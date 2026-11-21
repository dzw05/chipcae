// Consumer acceptance: JSON expectations are independent of the cutter output.
#include "chipcae/mesh/background/tensor_grid.hpp"
#include "chipcae/mesh/cut/detail/outline_nonconvex.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <jsoncons/json.hpp>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace {
using namespace chipcae;
using namespace chipcae::mesh;
using namespace chipcae::mesh::cut;
using Json = jsoncons::json;
using P2 = std::array<Real, 2>;
using P3 = std::array<Real, 3>;
using Ring = std::vector<P2>;
void check(bool ok, const std::string& context) {
    if (!ok)
        throw std::runtime_error(context);
}
void near(Real actual, Real expected, Real relative, const std::string& context) {
    std::ostringstream message;
    message << std::setprecision(17) << context << ": actual=" << actual
            << " expected=" << expected;
    check(std::isfinite(actual) && expected > 0 &&
              std::abs(actual - expected) <= relative * expected,
          message.str());
}
template <class T>
T value(Expected<T> result, const std::string& context) {
    if (!result)
        throw std::runtime_error(context + ": " + result.error().path + ": " +
                                 result.error().reason);
    return std::move(result.value());
}
Json read(const std::filesystem::path& path) {
    std::ifstream input(path);
    check(input.good(), "missing fixture: " + path.string());
    return Json::parse(input);
}
Real number(const Json& v) {
    Real result;
    if (v.is_string()) {
        const auto text = v.as<std::string>();
        std::size_t used = 0;
        result = std::stod(text, &used);
        check(used == text.size(), "malformed decimal: " + text);
    } else {
        check(v.is_number(), "fixture must contain a number or decimal string");
        result = v.as<Real>();
    }
    check(std::isfinite(result), "nonfinite fixture number");
    return result;
}
P3 point(const std::vector<Real>& xyz, std::size_t i) {
    return {xyz.at(3 * i), xyz.at(3 * i + 1), xyz.at(3 * i + 2)};
}
P3 difference(P3 a, P3 b) {
    for (int d = 0; d < 3; ++d)
        a[d] -= b[d];
    return a;
}
Real dot(P3 a, P3 b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
Real norm(P3 a) {
    return std::sqrt(dot(a, a));
}
struct Box {
    P3 lo, h;
    Real volume() const { return h[0] * h[1] * h[2]; }
    P3 physical(const std::array<std::vector<Real>, 3>& xi, std::size_t q) const {
        return {lo[0] + h[0] * xi[0].at(q), lo[1] + h[1] * xi[1].at(q), lo[2] + h[2] * xi[2].at(q)};
    }
};
Box bounds(const HexMeshArrays& m, std::size_t e) {
    const auto lo = point(m.vertices, m.hex.at(8 * e));
    return {lo, difference(point(m.vertices, m.hex.at(8 * e + 6)), lo)};
}
Real segment_distance(P2 p, P2 a, P2 b) {
    const Real dx = b[0] - a[0], dy = b[1] - a[1], length2 = dx * dx + dy * dy;
    const Real t =
        length2 > 0 ? std::clamp(((p[0] - a[0]) * dx + (p[1] - a[1]) * dy) / length2, 0., 1.) : 0;
    return std::hypot(p[0] - a[0] - t * dx, p[1] - a[1] - t * dy);
}
struct Fixture {
    Json input, expected;
    Ring ring;
    P3 lo{}, hi{};
    int nx = 8, ny = 8, nz = 1;
    explicit Fixture(const std::filesystem::path& directory)
        : input(read(directory / "input.json")), expected(read(directory / "expected.json")) {
        for (const auto& p : input.at("outline").array_range()) {
            check(p.size() == 2, "fixture outline coordinate dimension");
            ring.push_back({number(p.at(0)), number(p.at(1))});
        }
        check(ring.size() >= 3, "fixture outline has fewer than three vertices");
        for (int d = 0; d < 3; ++d) {
            lo[d] = number(input.at("mesh_box").at(0).at(d));
            hi[d] = number(input.at("mesh_box").at(1).at(d));
            check(hi[d] > lo[d], "fixture mesh box must be positive");
        }
        if (input.contains("grid")) {
            nx = input.at("grid").at("nx").as<int>();
            ny = input.at("grid").at("ny").as<int>();
            nz = input.at("grid").at("nz").as<int>();
        }
    }
    Footprint outline() const { return {{lo[0], lo[1], hi[0], hi[1]}, ring}; }
    CutOptions options() const {
        const auto& o = input.at("options");
        CutOptions options;
        options.export_polyhedra = o.at("export_polyhedra").as<bool>();
        options.keep_void = o.at("keep_void").as<bool>();
        options.quad_degree = o.at("quad_degree").as<int>();
        options.small_sub_vf = number(o.at("small_sub_vf"));
        return options;
    }
    HexMeshArrays background() const {
        StackModel stack;
        stack.id = "outline-fixture-consumer";
        stack.unit = "m";
        stack.outline = outline();
        stack.outline.polygon.clear(); // Build the full mesh_box, then cut the actual ring.
        check(std::abs(lo[2]) < 1e-30, "stack construction requires z origin zero");
        Layer layer;
        layer.name = "solid";
        layer.kind = "Explicit";
        layer.material = "synthetic";
        layer.thickness = hi[2] - lo[2];
        layer.n_elem_min = nz;
        layer.grading = 1;
        stack.layers = {layer};
        GridOptions o;
        o.nx = nx;
        o.ny = ny;
        o.levels = 1;
        o.max_ratio = 100;
        auto mesh = value(build_tensor_grid(stack, o), "fixture background");
        check(mesh.elem_gid.size() == static_cast<std::size_t>(nx * ny * nz),
              "fixture grid dimensions");
        return mesh;
    }
    // A decomposition endpoint can also be an exterior endpoint. Only reject
    // samples that cannot match any exterior segment with the outward normal.
    bool exterior(P3 p, P3 n) const {
        for (std::size_t i = 0; i < ring.size(); ++i) {
            const auto a = ring[i], b = ring[(i + 1) % ring.size()];
            P3 outward{b[1] - a[1], a[0] - b[0], 0};
            const Real length = norm(outward);
            if (!(length > 0))
                continue;
            for (auto& v : outward)
                v /= length;
            if (segment_distance({p[0], p[1]}, a, b) <= 1e-14 &&
                norm(difference(outward, n)) <= 1e-12)
                return true;
        }
        return false;
    }
};
struct Bits {
    std::vector<std::uint64_t> words;
    template <class T>
        requires std::is_arithmetic_v<T>
    void add(T v) {
        if constexpr (std::is_floating_point_v<T>) {
            words.push_back(std::bit_cast<std::uint64_t>(v));
        } else
            words.push_back(static_cast<std::uint64_t>(v));
    }
    void add(const std::string& s) {
        add(s.size());
        for (unsigned char c : s)
            add(c);
    }
    template <class T, std::size_t N>
    void add(const std::array<T, N>& a) {
        for (const auto& v : a)
            add(v);
    }
    template <class T>
    void add(const std::vector<T>& a) {
        add(a.size());
        for (const auto& v : a)
            add(v);
    }
    template <class K, class V>
    void add(const std::map<K, V>& a) {
        add(a.size());
        for (const auto& [k, v] : a) {
            add(k);
            add(v);
        }
    }
    void add(const AttributeRecord& a) {
        add(a.layer);
        add(a.region);
        add(a.material);
        add(a.body);
    }
    void add(const BodyLevelSet& a) {
        add(a.body);
        add(a.phi);
    }
    void add(const SnapReport& a) {
        add(a.axis);
        add(a.from);
        add(a.to);
        add(a.displacement);
    }
    void add(const HexMeshArrays::InterfaceReport& a) {
        add(a.name);
        add(a.side_a_bodies);
        add(a.side_b_bodies);
        add(a.aligned_faces);
        add(a.cut_faces);
        add(a.aligned_area);
        add(a.cut_area);
    }
    void add(const AttributeTable& a) {
        add(a.layers);
        add(a.entries);
        add(a.references);
        add(a.materials);
        add(a.bodies);
        add(a.boundary_names);
    }
    void add(const HexMeshArrays::GridReport& a) {
        add(a.levels);
        add(a.rule);
        add(a.N_L);
        add(a.auto_selected);
        add(a.padding);
        add(a.max_aspect_ratio);
        add(a.level_axes);
        add(a.column_ranks);
        add(a.warnings);
        add(a.partition_imbalance);
        add(a.void_cells);
    }
    void add(const HexMeshArrays& a) {
        add(a.vertices);
        add(a.hex);
        add(a.attribute);
        add(a.bdr_quad);
        add(a.bdr_attribute);
        add(a.elem_gid);
        add(a.node_gid);
        add(a.partitioning);
        add(a.node_layer);
        add(a.copper_layer_of_layer);
        add(a.copper_phi);
        add(a.node_column);
        add(a.attr_frame);
        add(a.nodes);
        add(a.internal_interfaces);
        add(a.order);
        add(a.preparation_report);
        add(a.grid_report);
        add(a.attr_table);
        add(a.face_sets);
        add(a.pending_cut);
        add(a.snapped);
        add(a.axes);
    }
    void add(const CutPolyhedra& a) {
        add(a.points);
        add(a.point_xi);
        add(a.point_parent_elem);
        add(a.face_conn);
        add(a.face_off);
        add(a.poly_faces);
        add(a.poly_off);
        add(a.parent_elem_gid);
        add(a.body);
        add(a.layer_id);
        add(a.material_id);
        add(a.cell_kind);
        add(a.volume);
        add(a.volfrac_ref);
    }
    void add(const CutLayerArrays& a) {
        add(a.polyhedra.has_value());
        if (a.polyhedra)
            add(*a.polyhedra);
        add(a.level_sets);
        add(a.cut_elem);
        add(a.sub_off);
        add(a.sub_attribute);
        add(a.sub_volfrac);
        add(a.vq_off);
        add(a.bq_off);
        add(a.vq_xi);
        add(a.bq_xi);
        add(a.bq_n);
        add(a.vq_w);
        add(a.bq_w);
        add(a.bq_bdr_attr);
        add(a.gp_elem_gid);
        add(a.gp_face);
        add(a.gp_face_gid);
        add(a.gp_face_minus);
        add(a.gp_face_plus);
        add(a.gp_owner_rank);
        add(a.face_sets);
        add(a.eb_points);
        add(a.eb_xi);
        add(a.eb_normals);
        add(a.eb_parent_elem);
        add(a.sq_off);
        add(a.sq_side);
        add(a.sq_xi);
        add(a.sq_w);
        add(a.void_attribute);
        add(a.cut_elem_gid);
        add(a.void_elem);
        add(a.small_subs);
        add(a.exterior_area_vector);
    }
};
template <class T>
void exact(const T& a, const T& b, std::string_view context) {
    Bits x, y;
    x.add(a);
    y.add(b);
    check(x.words == y.words, std::string(context) + ": recursive bit patterns differ");
}

struct Output {
    HexMeshArrays mesh;
    CutLayerArrays cut;
};
Output build(const Fixture& f, CutOptions o) {
    auto mesh = f.background();
    auto cut =
        value(chipcae::mesh::cut::detail::apply_outline_cut_general(mesh, f.outline(), o, {}),
              "general outline");
    check(cut.has_value(), "fixture must return cut arrays");
    return {std::move(mesh), std::move(*cut)};
}
void invariants(const Fixture& f, const Output& out) {
    const auto& m = out.mesh;
    const auto& c = out.cut;
    check(!c.cut_elem.empty(), "fixture must exercise cut cells");
    check(c.sub_off.size() == c.cut_elem.size() + 1 && c.sq_off.size() == c.cut_elem.size() + 1,
          "cut-cell CSR shape");
    std::set<Local> cut_cells(c.cut_elem.begin(), c.cut_elem.end());
    check(cut_cells.size() == c.cut_elem.size(), "cut cells must be unique");
    std::set<Local> void_cells(c.void_elem.begin(), c.void_elem.end());
    long double volume = 0;
    Real maximum_closure = 0;
    for (std::size_t e = 0; e < m.elem_gid.size(); ++e)
        if (!cut_cells.contains(static_cast<Local>(e)) &&
            !void_cells.contains(static_cast<Local>(e)))
            volume += bounds(m, e).volume();
    std::size_t internal_samples = 0, internal_faces = 0, embedded_samples = 0;
    check(c.eb_parent_elem.size() % 3 == 0, "embedded output contains complete triangles");
    for (std::size_t triangle = 0; triangle < c.eb_parent_elem.size(); triangle += 3) {
        bool exterior = true;
        for (std::size_t vertex = triangle; vertex < triangle + 3; ++vertex)
            exterior &= f.exterior(point(c.eb_points, vertex), point(c.eb_normals, vertex));
        if (!exterior)
            ++internal_faces;
    }
    for (std::size_t row = 0; row < c.cut_elem.size(); ++row) {
        const auto e = static_cast<std::size_t>(c.cut_elem.at(row));
        const auto box = bounds(m, e);
        for (Local sub = c.sub_off.at(row); sub < c.sub_off.at(row + 1); ++sub)
            volume += static_cast<long double>(box.volume()) * c.sub_volfrac.at(sub);
        struct Plane {
            P3 point, normal;
        };
        std::vector<Plane> planes;
        for (std::size_t i = 0; i < c.eb_parent_elem.size(); ++i) {
            if (c.eb_parent_elem[i] != static_cast<Local>(e))
                continue;
            auto n = point(c.eb_normals, i);
            const Real length = norm(n);
            check(length > 0, "embedded normal must be nonzero");
            for (auto& v : n)
                v /= length;
            planes.push_back({point(c.eb_points, i), n});
        }
        P3 area_vector{};
        Real face_area = 0;
        const Real plane_tol = 1e-12 * std::min({box.h[0], box.h[1], box.h[2]});
        for (Local q = c.sq_off.at(row); q < c.sq_off.at(row + 1); ++q) {
            const auto weight = c.sq_w.at(q);
            check(std::isfinite(weight) && weight >= 0, "finite positive face area weight");
            P3 normal{};
            const auto side = c.sq_side.at(q);
            if (side >= 0) {
                check(side < 6, "axial face side in [0,5]");
                normal[side / 2] = side % 2 ? 1 : -1;
            } else {
                check(side == -1, "embedded side must be -1");
                const auto p = box.physical(c.sq_xi, q);
                Real best = std::numeric_limits<Real>::infinity();
                for (const auto& plane : planes) {
                    const Real distance = std::abs(dot(plane.normal, difference(p, plane.point)));
                    if (distance < best) {
                        best = distance;
                        normal = plane.normal;
                    }
                }
                check(best <= plane_tol, "embedded sample has no outward EB triangle plane");
                ++embedded_samples;
                if (!f.exterior(p, normal))
                    ++internal_samples;
            }
            face_area += weight;
            for (int d = 0; d < 3; ++d)
                area_vector[d] += normal[d] * weight;
        }
        std::ostringstream context;
        context << std::setprecision(17) << "FVM area-vector closure gid=" << m.elem_gid.at(e)
                << " residual=" << norm(area_vector) << " face_area=" << face_area;
        check(face_area > 0 && norm(area_vector) <= 1e-12 * face_area, context.str());
        maximum_closure = std::max(maximum_closure, norm(area_vector) / face_area);
    }
    check(embedded_samples > 0 && !c.eb_parent_elem.empty(),
          "retain genuine exterior embedded faces");
    const auto expected_internal = f.expected.at("internal_side_minus1_faces").as<std::size_t>();
    check(internal_samples == expected_internal, "decomposition-only embedded samples");
    check(internal_faces == expected_internal, "decomposition-only embedded faces");
    near(static_cast<Real>(volume), number(f.expected.at("volume")), 1e-12, "fixture volume");
    std::cout << "volume=" << std::setprecision(17) << volume
              << " embedded samples=" << embedded_samples
              << " decomposition faces=" << internal_faces
              << " max closure relative=" << maximum_closure << " volume relative error="
              << std::abs(static_cast<Real>(volume) / number(f.expected.at("volume")) - 1) << '\n';
}
void l1(const Fixture& f, const Output& out, Real threshold) {
    const auto& m = out.mesh;
    const auto& c = out.cut;
    const auto original = f.background();
    std::map<Index, P3> original_lo;
    for (std::size_t e = 0; e < original.elem_gid.size(); ++e)
        original_lo.emplace(original.elem_gid[e], bounds(original, e).lo);
    std::set<Index> matched;
    std::size_t corners = 0, strips = 0;
    Real maximum_fraction_error = 0;
    for (const auto& cell : f.expected.at("cut_cells").array_range()) {
        P3 target{};
        for (int d = 0; d < 3; ++d) {
            const int count = d == 0 ? f.nx : d == 1 ? f.ny : f.nz;
            target[d] = f.lo[d] + (f.hi[d] - f.lo[d]) * cell.at("cell").at(d).as<int>() / count;
        }
        bool found = false;
        for (std::size_t row = 0; row < c.cut_elem.size(); ++row) {
            const auto e = static_cast<std::size_t>(c.cut_elem[row]);
            const auto lo = bounds(m, e).lo;
            if (norm(difference(lo, target)) > 1e-14)
                continue;
            check(!found && matched.insert(m.elem_gid[e]).second,
                  "L1 unique coordinate and stable GID");
            found = true;
            check(norm(difference(original_lo.at(m.elem_gid[e]), lo)) < 1e-14,
                  "GID stable after compaction");
            check(c.sub_off[row + 1] - c.sub_off[row] == 1, "L1 union has one subcell");
            const Real expected = number(cell.at("sub_volfrac"));
            near(c.sub_volfrac.at(c.sub_off[row]), expected, 1e-10, "L1 fixture cell fraction");
            maximum_fraction_error = std::max(
                maximum_fraction_error, std::abs(c.sub_volfrac.at(c.sub_off[row]) / expected - 1));
            if (std::abs(expected - number(f.expected.at("corner_fraction"))) < 1e-14)
                ++corners;
            else {
                near(expected,
                     number(f.expected.at("strip_fraction")),
                     1e-10,
                     "L1 strip expectation");
                ++strips;
            }
        }
        check(found, "L1 expected cell disappeared after compaction");
    }
    check(corners == f.expected.at("corner_count").as<std::size_t>() &&
              strips == f.expected.at("strip_count").as<std::size_t>(),
          "L1 exact corner and strip counts");
    std::set<Local> small(c.small_subs.begin(), c.small_subs.end());
    check(small.size() == c.small_subs.size(), "small_subs IDs unique");
    for (const auto id : small)
        check(id >= 0 && static_cast<std::size_t>(id) < c.sub_volfrac.size(),
              "small_subs ID range");
    for (std::size_t sub = 0; sub < c.sub_volfrac.size(); ++sub)
        check(small.contains(static_cast<Local>(sub)) == (c.sub_volfrac[sub] < threshold),
              "each qualifying subcell once, each nonqualifying subcell zero times");
    const auto key = threshold > .00075 ? "0.001" : "0.0005";
    check(small.size() == f.expected.at("small_subs").at(key).as<std::size_t>(),
          "fixture small_subs count");
    std::cout << "L1 threshold=" << threshold << " small_subs=" << small.size()
              << " max fraction relative error=" << maximum_fraction_error << '\n';
}
void run(const std::filesystem::path& root, const std::string& name) {
    const Fixture f(root / name);
    if (name == "chamfer_lid_octagon") {
        auto legacy = f.background(), general = f.background();
        const auto a =
            value(apply_outline_cut(legacy, f.outline(), f.options(), {}), "legacy chamfer");
        const auto b = value(chipcae::mesh::cut::detail::apply_outline_cut_general(
                                 general, f.outline(), f.options(), {}),
                             "general chamfer");
        check(a.has_value() && b.has_value(), "chamfer cut arrays exist");
        exact(legacy, general, "chamfer mesh every field");
        exact(*a, *b, "chamfer cut every field");
        invariants(f, {std::move(general), *b});
    } else if (name == "l1_corner") {
        for (const Real threshold : {.001, .0005}) {
            auto options = f.options();
            options.small_sub_vf = threshold;
            const auto out = build(f, options);
            invariants(f, out);
            l1(f, out, threshold);
        }
    } else {
        invariants(f, build(f, f.options()));
    }
}
} // namespace
int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "expected fixture directory argument\n";
        return 1;
    }
    int failures = 0;
    for (const std::string name :
         {"l_shape", "v_shape", "r_shape", "l1_corner", "chamfer_lid_octagon"}) {
        try {
            run(argv[1], name);
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    }
    return failures == 0 ? 0 : 1;
}
