#include "chipcae/core/types.hpp"
#include "q2_export.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <grp.h>
#include <iostream>
#include <jsoncons/json.hpp>
#include <limits>
#include <map>
#include <mfem.hpp>
#include <numeric>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {
using Json = jsoncons::json;
using Point = std::array<double, 3>;
using Values = std::array<double, 4>;
using chipcae::solve::detail::q2_export_requested;
using chipcae::solve::detail::write_q2_export;
constexpr double eps = std::numeric_limits<double>::epsilon();
constexpr std::array<std::array<int, 3>, 11> powers{{{0, 0, 0},
                                                     {1, 0, 0},
                                                     {0, 1, 0},
                                                     {0, 0, 1},
                                                     {2, 0, 0},
                                                     {0, 2, 0},
                                                     {0, 0, 2},
                                                     {1, 1, 0},
                                                     {0, 1, 1},
                                                     {1, 0, 1},
                                                     {2, 2, 2}}};
// Four independent fields; every monomial has a nonzero coefficient.
constexpr std::array<std::array<double, 11>, 4> coefficients{
    {{1.3, -.7, .9, 1.1, -.6, .8, 1.7, -.4, .3, -.9, 2.1},
     {-.8, 1.2, -.5, .6, .7, -1.3, .4, 1.9, -.2, .8, -1.4},
     {.4, -1.6, .3, -.9, 1.4, .2, -.8, -.7, 1.2, .5, 3.3},
     {310.7, 2.3, -3.1, 4.9, -.8, 1.6, -2.2, .7, -1.4, 2.8, -4.1}}};

void require(bool ok, const std::string& message) {
    if (!ok)
        throw std::runtime_error(message);
}
struct Failure : std::runtime_error {
    std::string code;
    Failure(std::string c, const std::string& detail)
        : std::runtime_error(c + ": " + detail), code(std::move(c)) {}
};
void check(bool ok, const std::string& code, const std::string& detail) {
    if (!ok)
        throw Failure(code, detail);
}
double power(double v, int n) {
    double result = 1;
    for (int i = 0; i < n; ++i)
        result *= v;
    return result;
}
Values exact(const Point& p) {
    Values value{};
    for (std::size_t c = 0; c < 4; ++c)
        for (std::size_t m = 0; m < powers.size(); ++m) {
            double term = coefficients[c][m];
            for (std::size_t d = 0; d < 3; ++d)
                term *= power(p[d], powers[m][d]);
            value[c] += term;
        }
    return value;
}
Values tolerances() {
    // Bounds come only from analytic coefficients and the prescribed box.
    constexpr Point magnitude{1., .3, .05};
    constexpr double X = 1.;
    Values result{};
    for (std::size_t c = 0; c < 4; ++c) {
        double S = 0, G = 0;
        for (std::size_t m = 0; m < powers.size(); ++m) {
            double bound = std::abs(coefficients[c][m]);
            for (std::size_t d = 0; d < 3; ++d)
                bound *= power(magnitude[d], powers[m][d]);
            S += bound;
            double gradient_squared = 0;
            for (std::size_t d = 0; d < 3; ++d) {
                if (powers[m][d] == 0)
                    continue;
                double derivative = std::abs(coefficients[c][m]) * powers[m][d];
                for (std::size_t a = 0; a < 3; ++a)
                    derivative *= power(magnitude[a], powers[m][a] - (a == d ? 1 : 0));
                gradient_squared += derivative * derivative;
            }
            G += std::sqrt(gradient_squared);
        }
        result[c] = 256 * eps * (S + G * X);
    }
    return result;
}

struct TemporaryDirectory {
    std::filesystem::path path;
    TemporaryDirectory() {
        auto pattern = (std::filesystem::temp_directory_path() / "q2-unit-XXXXXX").string();
        std::vector<char> buffer(pattern.begin(), pattern.end());
        buffer.push_back('\0');
        const char* created = mkdtemp(buffer.data());
        require(created != nullptr, "mkdtemp failed");
        path = created;
        std::filesystem::permissions(
            path,
            std::filesystem::perms::owner_all | std::filesystem::perms::group_read |
                std::filesystem::perms::group_exec | std::filesystem::perms::others_read |
                std::filesystem::perms::others_exec);
    }
    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

struct Element {
    std::int64_t gid{};
    std::array<Point, 27> points{}, U{};
    std::array<double, 27> T{};
    std::array<std::array<double, 3>, 3> levels{};
    std::array<int, 27> order{};
};
using Documents = std::vector<Json>;

bool keys(const Json& object, const std::set<std::string>& wanted) {
    if (!object.is_object())
        return false;
    std::set<std::string> actual;
    for (const auto& member : object.object_range())
        actual.insert(std::string(member.key()));
    return actual == wanted;
}
std::vector<Element> structure(const Documents& documents, bool strict) {
    std::set<std::int64_t> gids;
    std::vector<Element> result;
    for (const auto& doc : documents) {
        check(keys(doc, {"elements"}) && doc.at("elements").is_array(), "KEYS", "document");
        bool first = true;
        std::int64_t previous = 0;
        for (const auto& raw : doc.at("elements").array_range()) {
            check(keys(raw, {"gid", "points", "U", "T"}), "KEYS", "element");
            check(raw.at("gid").is_int64() || raw.at("gid").is_uint64(), "GID", "integer");
            Element e;
            e.gid = raw.at("gid").as<std::int64_t>();
            check(gids.insert(e.gid).second, "DUP_GID", "across ranks");
            check(first || e.gid > previous, "ORDER", "gids strictly increasing");
            first = false;
            previous = e.gid;
            for (const auto* field : {"points", "U", "T"})
                check(raw.at(field).is_array() && raw.at(field).size() == 27, "COUNT", field);
            std::set<Point> unique;
            std::array<std::set<double>, 3> axes;
            double X = 0;
            for (std::size_t q = 0; q < 27; ++q) {
                check(raw.at("points")[q].size() == 3 && raw.at("U")[q].size() == 3,
                      "COUNT",
                      "vector shape");
                for (std::size_t d = 0; d < 3; ++d) {
                    e.points[q][d] = raw.at("points")[q][d].as<double>();
                    e.U[q][d] = raw.at("U")[q][d].as<double>();
                    check(std::isfinite(e.points[q][d]) && std::isfinite(e.U[q][d]),
                          "NONFINITE",
                          "point/U");
                    axes[d].insert(e.points[q][d]);
                    X = std::max(X, std::abs(e.points[q][d]));
                }
                e.T[q] = raw.at("T")[q].as<double>();
                check(std::isfinite(e.T[q]), "NONFINITE", "T");
                check(unique.insert(e.points[q]).second, "DUP_NODE", "repeated coordinates");
            }
            for (std::size_t d = 0; d < 3; ++d) {
                std::vector<double> levels;
                for (double coordinate : axes[d])
                    if (levels.empty() || std::abs(coordinate - levels.back()) > 4 * eps * X)
                        levels.push_back(coordinate);
                check(levels.size() == 3, "GRID", "three levels per axis within 4*eps*X");
                std::copy(levels.begin(), levels.end(), e.levels[d].begin());
                check(std::abs(e.levels[d][1] - (e.levels[d][0] + e.levels[d][2]) / 2) <=
                          4 * eps * X,
                      "GRID",
                      "midpoint");
            }
            std::set<int> indices;
            for (std::size_t q = 0; q < 27; ++q) {
                int index = 0, stride = 1;
                for (std::size_t d = 0; d < 3; ++d) {
                    const auto found = std::min_element(
                        e.levels[d].begin(), e.levels[d].end(), [&](double a, double b) {
                            return std::abs(a - e.points[q][d]) < std::abs(b - e.points[q][d]);
                        });
                    check(std::abs(*found - e.points[q][d]) <= 4 * eps * X, "GRID", "axis level");
                    index += stride * static_cast<int>(found - e.levels[d].begin());
                    stride *= 3;
                }
                check(indices.insert(index).second, "DUP_NODE", "tensor index");
                e.order[q] = index;
                if (strict)
                    check(index == static_cast<int>(q), "ORDER", "q=i+3j+9k");
            }
            result.push_back(e);
        }
    }
    return result;
}
Values reconstruct(const Element& e, const Point& p) {
    std::array<Point, 3> basis{};
    for (std::size_t d = 0; d < 3; ++d) {
        const double r = (p[d] - e.levels[d][0]) / (e.levels[d][2] - e.levels[d][0]);
        basis[d] = {2 * (r - .5) * (r - 1), 4 * r * (1 - r), 2 * r * (r - .5)};
    }
    Values v{};
    for (std::size_t q = 0; q < 27; ++q) {
        const int index = e.order[q];
        const double weight = basis[0][index % 3] * basis[1][(index / 3) % 3] * basis[2][index / 9];
        for (std::size_t d = 0; d < 3; ++d)
            v[d] += weight * e.U[q][d];
        v[3] += weight * e.T[q];
    }
    return v;
}

// Fixed plane x+.37y=.53, chosen before any exporter execution. Rectangle clipping
// plus the shoelace centroid gives the physical prism centroid without MFEM.
std::vector<Point> cut_centroid(const Point& lo, const Point& hi) {
    std::vector<Point> polygon{
        {lo[0], lo[1], 0}, {hi[0], lo[1], 0}, {hi[0], hi[1], 0}, {lo[0], hi[1], 0}};
    std::vector<Point> clipped;
    bool inside = false, outside = false;
    for (std::size_t i = 0; i < polygon.size(); ++i) {
        const auto& p = polygon[i];
        const auto& q = polygon[(i + 1) % polygon.size()];
        const double a = p[0] + .37 * p[1] - .53, b = q[0] + .37 * q[1] - .53;
        require(std::abs(a) > 16 * eps, "clipping plane intersects grid vertex");
        inside = inside || a < 0;
        outside = outside || a > 0;
        if (a < 0)
            clipped.push_back(p);
        if ((a < 0) != (b < 0)) {
            const double f = a / (a - b);
            clipped.push_back({p[0] + f * (q[0] - p[0]), p[1] + f * (q[1] - p[1]), 0});
        }
    }
    if (!(inside && outside))
        return {};
    double twice_area = 0;
    Point centroid{};
    for (std::size_t i = 0; i < clipped.size(); ++i) {
        const auto& p = clipped[i];
        const auto& q = clipped[(i + 1) % clipped.size()];
        const double cross = p[0] * q[1] - q[0] * p[1];
        twice_area += cross;
        for (std::size_t d = 0; d < 2; ++d)
            centroid[d] += (p[d] + q[d]) * cross;
    }
    const double fraction = twice_area / (2 * (hi[0] - lo[0]) * (hi[1] - lo[1]));
    require(fraction >= .1 && fraction <= .9, "clipped prism fraction outside [0.1,0.9]");
    for (std::size_t d = 0; d < 2; ++d)
        centroid[d] /= 3 * twice_area;
    centroid[2] = (lo[2] + hi[2]) / 2;
    return {centroid};
}

Json point_json(const Point& point) {
    Json j = Json::array();
    for (double x : point)
        j.push_back(x);
    return j;
}
Json element_json(const Element& e) {
    Json j;
    j["gid"] = e.gid;
    for (const auto* name : {"points", "U", "T"})
        j[name] = Json::array();
    for (std::size_t q = 0; q < 27; ++q) {
        j["points"].push_back(point_json(e.points[q]));
        j["U"].push_back(point_json(e.U[q]));
        j["T"].push_back(e.T[q]);
    }
    return j;
}

struct Fixture {
    mfem::Mesh mesh;
    mfem::H1_FECollection collection{2, 3};
    mfem::FiniteElementSpace scalar, vector;
    mfem::GridFunction U, T;
    std::vector<chipcae::Index> gids;
    std::vector<int> ranks;
    std::array<std::vector<double>, 3> nodal_axes;
    std::map<std::int64_t, Element> captured;
    std::map<std::int64_t, std::vector<Point>> samples;
    Documents oracle = Documents(2);

    explicit Fixture(bool nonuniform)
        : mesh(mfem::Mesh::MakeCartesian3D(3, 2, 4, mfem::Element::HEXAHEDRON, .8, .4, .05)),
          scalar(&mesh, &collection), vector(&mesh, &collection, 3, mfem::Ordering::byVDIM),
          U(&vector), T(&scalar) {
        const std::array<std::vector<double>, 3> axes =
            nonuniform ? std::array<std::vector<double>, 3>{{{.2, .4, .7, 1},
                                                             {-.3, -.12, .1},
                                                             {0, .002, .01, .026, .05}}}
                       : std::array<std::vector<double>, 3>{{{.2, .2 + .8 / 3, .2 + 1.6 / 3, 1},
                                                             {-.3, -.1, .1},
                                                             {0, .0125, .025, .0375, .05}}};
        for (std::size_t d = 0; d < 3; ++d) {
            for (std::size_t i = 0; i + 1 < axes[d].size(); ++i) {
                nodal_axes[d].push_back(axes[d][i]);
                nodal_axes[d].push_back((axes[d][i] + axes[d][i + 1]) / 2);
            }
            nodal_axes[d].push_back(axes[d].back());
        }
        const Point initial_size{.8, .4, .05};
        constexpr std::array<int, 3> counts{3, 2, 4};
        for (int v = 0; v < mesh.GetNV(); ++v) {
            double* p = mesh.GetVertex(v);
            for (std::size_t d = 0; d < 3; ++d) {
                const auto index =
                    static_cast<std::size_t>(std::lround(p[d] / initial_size[d] * counts[d]));
                p[d] = axes[d].at(index);
            }
        }
        mfem::VectorFunctionCoefficient displacement(3, [](const mfem::Vector& p, mfem::Vector& u) {
            const auto values = exact({p[0], p[1], p[2]});
            for (int d = 0; d < 3; ++d)
                u[d] = values[static_cast<std::size_t>(d)];
        });
        mfem::FunctionCoefficient temperature(
            [](const mfem::Vector& p) { return exact({p[0], p[1], p[2]})[3]; });
        U.ProjectCoefficient(displacement);
        T.ProjectCoefficient(temperature);
        require(scalar.GetNDofs() == 315, "prescribed topology NDofs != 315");
        for (auto& doc : oracle)
            doc["elements"] = Json::array();
        std::mt19937 random(0x514232u);
        std::uniform_real_distribution<double> uniform(.05, .95);
        constexpr std::array<Point, 8> fixed{{{.5, .5, .5},
                                              {.13, .27, .39},
                                              {.81, .17, .63},
                                              {.23, .89, .41},
                                              {.71, .77, .19},
                                              {.31, .47, .91},
                                              {.93, .61, .29},
                                              {.07, .83, .73}}};
        for (int e = 0; e < mesh.GetNE(); ++e) {
            gids.push_back(23 - e); // Descending, interleaved ownership: sorting is observable.
            ranks.push_back((e % 4 == 0 || e % 4 == 3) ? 1 : 0);
            Element expected;
            expected.gid = gids.back();
            // Capture independently at explicit tensor reference nodes, without GetNodes sorting.
            for (int k = 0; k < 3; ++k)
                for (int j = 0; j < 3; ++j)
                    for (int i = 0; i < 3; ++i) {
                        const auto q = static_cast<std::size_t>(i + 3 * j + 9 * k);
                        mfem::IntegrationPoint ip;
                        ip.Set3(.5 * i, .5 * j, .5 * k);
                        mfem::Vector p(3), u(3);
                        mesh.GetElementTransformation(e)->Transform(ip, p);
                        U.GetVectorValue(e, ip, u);
                        for (std::size_t d = 0; d < 3; ++d) {
                            expected.points[q][d] = p[static_cast<int>(d)];
                            expected.U[q][d] = u[static_cast<int>(d)];
                        }
                        expected.T[q] = T.GetValue(e, ip);
                    }
            const auto lo = expected.points.front(), hi = expected.points.back();
            auto& points = samples[expected.gid];
            points = cut_centroid(lo, hi);
            for (const auto& ref : fixed) {
                Point p;
                for (std::size_t d = 0; d < 3; ++d)
                    p[d] = lo[d] + ref[d] * (hi[d] - lo[d]);
                points.push_back(p);
            }
            for (int n = 0; n < 4; ++n) {
                Point p;
                for (std::size_t d = 0; d < 3; ++d)
                    p[d] = lo[d] + uniform(random) * (hi[d] - lo[d]);
                points.push_back(p);
            }
            // Include nodes as well as the required interior/centroid samples.
            points.insert(points.end(), expected.points.begin(), expected.points.end());
            captured.emplace(expected.gid, expected);
        }
        for (const auto& [gid, e] : captured) {
            const auto index = static_cast<std::size_t>(23 - gid);
            oracle[static_cast<std::size_t>(ranks[index])]["elements"].push_back(element_json(e));
        }
    }

    Values errors(const std::vector<Element>& elements) const {
        Values maximum{};
        for (const auto& e : elements)
            for (const auto& p : samples.at(e.gid)) {
                const auto value = reconstruct(e, p), analytic = exact(p);
                for (std::size_t c = 0; c < 4; ++c)
                    maximum[c] = std::max(maximum[c], std::abs(value[c] - analytic[c]));
            }
        return maximum;
    }
    void values(const Documents& docs, bool strict = true) const {
        const auto maximum = errors(structure(docs, strict)), limits = tolerances();
        for (std::size_t c = 0; c < 4; ++c) {
            std::cout << "实测 component=" << c << " maximum=" << maximum[c]
                      << " limit=" << limits[c] << '\n';
            check(maximum[c] <= limits[c],
                  "VALUE",
                  "analytic reconstruction component=" + std::to_string(c));
        }
    }
    auto write(const std::filesystem::path& file, int rank) {
        return write_q2_export(file, mesh, scalar, U, T, gids, ranks, rank);
    }
};

void expect_code(const std::string& wanted, const std::function<void()>& operation) {
    try {
        operation();
    } catch (const Failure& failure) {
        require(failure.code == wanted, "expected " + wanted + ", got " + failure.what());
        return;
    }
    throw std::runtime_error("corrupt input accepted: expected " + wanted);
}
void checker_controls(Fixture& f) {
    f.values(f.oracle);
    auto bad = f.oracle;
    std::swap(bad[0]["elements"][0]["points"][0], bad[0]["elements"][0]["points"][13]);
    expect_code("ORDER", [&] { (void)structure(bad, true); });
    expect_code("VALUE", [&] { f.values(bad, false); });
    const auto error = f.errors(structure(bad, false)), limit = tolerances();
    require(error[0] > 1e6 * limit[0], "N1 reconstruction error not > 1e6*tol");
    bad = f.oracle;
    std::swap(bad[0]["elements"][0]["U"][1], bad[0]["elements"][0]["U"][2]);
    expect_code("VALUE", [&] { f.values(bad); });
    bad = f.oracle;
    for (const auto* field : {"points", "U", "T"})
        bad[0]["elements"][0][field].erase(bad[0]["elements"][0][field].array_range().end() - 1);
    expect_code("COUNT", [&] { (void)structure(bad, true); });
    bad = f.oracle;
    bad[0]["elements"][0]["points"][5] = bad[0]["elements"][0]["points"][4];
    expect_code("DUP_NODE", [&] { (void)structure(bad, true); });
    bad = f.oracle;
    bad[1]["elements"][0]["gid"] = bad[0]["elements"][0]["gid"];
    expect_code("DUP_GID", [&] { (void)structure(bad, true); });
    bad = f.oracle;
    bad[0]["elements"][0]["U"][0][0] = std::numeric_limits<double>::quiet_NaN();
    expect_code("NONFINITE", [&] { (void)structure(bad, true); });
    bad = f.oracle;
    for (const auto* field : {"points", "U", "T"})
        std::swap(bad[0]["elements"][0][field][0], bad[0]["elements"][0][field][13]);
    f.values(bad, false);
    expect_code("ORDER", [&] { (void)structure(bad, true); });
}

void export_and_check(Fixture& f, const std::filesystem::path& folder) {
    Documents docs;
    for (int rank = 0; rank < 2; ++rank) {
        const auto file = folder / ("accuracy-q2-rank" + std::to_string(rank) + ".json");
        const auto stats = f.write(file, rank);
        require(stats.elements == 12, "export must report 12 owned elements per fake rank");
        require(stats.nodes == 27 * stats.elements, "stats.nodes != 27*elements");
        require(std::filesystem::exists(file), "export did not publish final file");
        require(stats.bytes == std::filesystem::file_size(file), "stats.bytes != actual file size");
        require(std::isfinite(stats.seconds) && stats.seconds >= 0, "invalid export timing");
        std::ifstream stream(file);
        docs.push_back(Json::parse(stream));
        for (const auto& owned : docs.back().at("elements").array_range()) {
            const auto gid = owned.at("gid").as<std::int64_t>();
            require(gid >= 0 && gid < 24, "exported gid out of range");
            require(f.ranks.at(static_cast<std::size_t>(23 - gid)) == rank,
                    "element written by incorrect rank");
        }
        std::cout << "实测 rank=" << rank << " bytes=" << stats.bytes
                  << " seconds=" << stats.seconds << '\n';
    }
    const auto elements = structure(docs, true);
    require(elements.size() == 24, "exported element count != 24");
    std::set<std::array<std::size_t, 3>> nodes;
    std::set<std::int64_t> gids;
    for (const auto& e : elements) {
        gids.insert(e.gid);
        const auto& expected = f.captured.at(e.gid);
        for (std::size_t q = 0; q < 27; ++q) {
            std::array<std::size_t, 3> grid_index;
            for (std::size_t d = 0; d < 3; ++d) {
                const auto& axis = f.nodal_axes[d];
                const auto found =
                    std::min_element(axis.begin(), axis.end(), [&](double a, double b) {
                        return std::abs(a - e.points[q][d]) < std::abs(b - e.points[q][d]);
                    });
                require(std::abs(*found - e.points[q][d]) <= 4 * eps,
                        "coordinate differs from independently prescribed grid by >4*eps*X");
                grid_index[d] = static_cast<std::size_t>(found - axis.begin());
            }
            nodes.insert(grid_index);
            for (std::size_t d = 0; d < 3; ++d) {
                require(std::memcmp(&e.points[q][d], &expected.points[q][d], sizeof(double)) == 0,
                        "points double failed bitwise round-trip");
                require(std::memcmp(&e.U[q][d], &expected.U[q][d], sizeof(double)) == 0,
                        "U double failed bitwise round-trip");
            }
            require(std::memcmp(&e.T[q], &expected.T[q], sizeof(double)) == 0,
                    "T double failed bitwise round-trip");
        }
    }
    require(nodes.size() == 315 && nodes.size() == static_cast<std::size_t>(f.scalar.GetNDofs()),
            "unique Q2 nodes != 315/NDofs");
    for (std::int64_t gid = 0; gid < 24; ++gid)
        require(gids.contains(gid), "missing gid in 0..23 union");
    f.values(docs);
    // Repeat corruptions on actual readback, not only analytic checker controls.
    auto oracle = f.oracle;
    f.oracle = docs;
    checker_controls(f);
    f.oracle = std::move(oracle);
}

template <class Callable>
void must_throw(Callable&& operation, const std::filesystem::path& file) {
    bool threw = false;
    try {
        operation();
    } catch (const std::exception&) {
        threw = true;
    }
    require(threw, "exporter must throw on invalid input or unwritable destination");
    require(!std::filesystem::exists(file), "failed export published a partial final file");
}
void permission_failure(Fixture& f, const std::filesystem::path& folder) {
    const auto locked = folder / "read-only";
    std::filesystem::create_directory(locked);
    std::filesystem::permissions(
        locked,
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec |
            std::filesystem::perms::group_read | std::filesystem::perms::group_exec |
            std::filesystem::perms::others_read | std::filesystem::perms::others_exec);
    const auto file = locked / "accuracy-q2-rank0.json";
    const pid_t child = fork();
    require(child >= 0, "fork failed");
    if (child == 0) {
        try {
            if (geteuid() == 0) {
                require(setgroups(0, nullptr) == 0, "setgroups failed");
                require(setgid(65534) == 0 && setuid(65534) == 0, "privilege drop failed");
            }
            must_throw([&] { (void)f.write(file, 0); }, file);
            _exit(0);
        } catch (const std::exception& error) {
            std::cerr << "permission child: " << error.what() << std::endl;
            _exit(1);
        }
    }
    int status = 0;
    require(waitpid(child, &status, 0) == child, "waitpid failed");
    std::filesystem::permissions(locked, std::filesystem::perms::owner_all);
    require(WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "unwritable destination assertion failed");
    require(!std::filesystem::exists(file), "unwritable export published final file");
}
void environment() {
    const char* original = std::getenv("CHIPCAE_ACCURACY_Q2_EXPORT");
    const bool present = original != nullptr;
    const std::string saved = original ? original : "";
    struct Restore {
        bool present;
        std::string saved;
        ~Restore() {
            if (present)
                setenv("CHIPCAE_ACCURACY_Q2_EXPORT", saved.c_str(), 1);
            else
                unsetenv("CHIPCAE_ACCURACY_Q2_EXPORT");
        }
    } restore{present, saved};
    require(unsetenv("CHIPCAE_ACCURACY_Q2_EXPORT") == 0, "unsetenv failed");
    require(!q2_export_requested(), "unset must disable export");
    for (const auto* value : {"", "0", "true", "1"}) {
        require(setenv("CHIPCAE_ACCURACY_Q2_EXPORT", value, 1) == 0, "setenv failed");
        require(q2_export_requested() == (std::string(value) == "1"),
                "q2_export_requested must accept exactly 1; value='" + std::string(value) + "'");
    }
}
} // namespace

int main() {
    int failures = 0;
    auto run = [&](const std::string& name, auto&& test) {
        try {
            test();
            std::cout << "PASS " << name << std::endl;
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL " << name << ": " << error.what() << std::endl;
        }
    };
    run("exact environment switch", environment);
    for (bool nonuniform : {false, true}) {
        Fixture f(nonuniform);
        TemporaryDirectory folder;
        run(nonuniform ? "nonuniform checker controls N1-N6/P1"
                       : "uniform checker controls N1-N6/P1",
            [&] { checker_controls(f); });
        run(nonuniform ? "nonuniform Q2 export" : "uniform Q2 export",
            [&] { export_and_check(f, folder.path); });
        run("empty ownership", [&] {
            const auto file = folder.path / "empty.json";
            const auto stats = f.write(file, 7);
            require(stats.elements == 0 && stats.nodes == 0, "empty rank counts");
            require(std::filesystem::exists(file), "empty rank must publish {\"elements\":[]}");
            std::ifstream stream(file);
            const auto json = Json::parse(stream);
            require(keys(json, {"elements"}) && json.at("elements").is_array() &&
                        json.at("elements").empty(),
                    "empty rank JSON");
            require(stats.bytes == std::filesystem::file_size(file) &&
                        std::isfinite(stats.seconds) && stats.seconds >= 0,
                    "empty rank stats");
        });
        run("nonfinite input", [&] {
            const double saved = f.U[0];
            f.U[0] = std::numeric_limits<double>::quiet_NaN();
            const auto file = folder.path / "nan.json";
            try {
                must_throw([&] { (void)f.write(file, f.ranks[0]); }, file);
            } catch (...) {
                f.U[0] = saved;
                throw;
            }
            f.U[0] = saved;
        });
        run("unwritable destination (drop root privileges)",
            [&] { permission_failure(f, folder.path); });
        run("malformed arrays", [&] {
            auto gids = f.gids;
            gids.pop_back();
            const auto file = folder.path / "malformed.json";
            must_throw(
                [&] { (void)write_q2_export(file, f.mesh, f.scalar, f.U, f.T, gids, f.ranks, 0); },
                file);
        });
        run("malformed partition length", [&] {
            auto ranks = f.ranks;
            ranks.pop_back();
            const auto file = folder.path / "partition-length.json";
            must_throw(
                [&] { (void)write_q2_export(file, f.mesh, f.scalar, f.U, f.T, f.gids, ranks, 0); },
                file);
        });
        run("duplicate owned GID rejected", [&] {
            auto gids = f.gids;
            gids[2] = gids[1]; // elements 1 and 2 are both owned by fake rank 0.
            const auto file = folder.path / "duplicate-gid.json";
            must_throw(
                [&] { (void)write_q2_export(file, f.mesh, f.scalar, f.U, f.T, gids, f.ranks, 0); },
                file);
        });
        run("64-bit integer GID preserved", [&] {
            auto gids = f.gids;
            constexpr std::int64_t large = 9007199254740993LL;
            gids[0] = large;
            const auto file = folder.path / "large-gid.json";
            const auto stats = write_q2_export(file, f.mesh, f.scalar, f.U, f.T, gids, f.ranks, 1);
            require(stats.elements == 12 && std::filesystem::exists(file),
                    "large GID export missing");
            std::ifstream stream(file);
            const auto doc = Json::parse(stream);
            const auto elements = structure({doc}, true);
            require(elements.back().gid == large, "GID rounded through floating point");
        });
        run("failed replacement preserves complete destination", [&] {
            const auto file = folder.path / "preserved.json";
            const std::string sentinel = "previous complete destination";
            {
                std::ofstream stream(file);
                stream << sentinel;
            }
            auto gids = f.gids;
            gids.pop_back();
            bool threw = false;
            try {
                (void)write_q2_export(file, f.mesh, f.scalar, f.U, f.T, gids, f.ranks, 0);
            } catch (const std::exception&) {
                threw = true;
            }
            require(threw, "invalid replacement must throw");
            std::ifstream stream(file);
            const std::string contents((std::istreambuf_iterator<char>(stream)),
                                       std::istreambuf_iterator<char>());
            require(contents == sentinel, "failure replaced complete destination");
        });
        run("non-Q2 rejected", [&] {
            mfem::H1_FECollection linear(1, 3);
            mfem::FiniteElementSpace scalar(&f.mesh, &linear), vector(&f.mesh, &linear, 3);
            mfem::GridFunction U(&vector), T(&scalar);
            U = 0.;
            T = 300.;
            const auto file = folder.path / "linear.json";
            must_throw(
                [&] { (void)write_q2_export(file, f.mesh, scalar, U, T, f.gids, f.ranks, 0); },
                file);
        });
    }
    run("peak RSS", [] {
        rusage usage{};
        require(getrusage(RUSAGE_SELF, &usage) == 0, "getrusage failed");
#ifdef __APPLE__
        const double rss = static_cast<double>(usage.ru_maxrss) / 1048576.;
#else
        const double rss = static_cast<double>(usage.ru_maxrss) / 1024.;
#endif
        std::cout << "实测 peak_RSS_MiB=" << rss << " limit=15360\n";
        require(rss <= 15360, "RSS exceeds 15360 MiB");
    });
    return failures == 0 ? 0 : 1;
}
