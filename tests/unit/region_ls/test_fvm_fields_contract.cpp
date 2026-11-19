#include "chipcae/mesh/cut/cut_stats.hpp"
#include "chipcae/mesh/cut/outline_cut.hpp"
#include "support.hpp"

#include <bit>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <map>
#include <string>

namespace {
using namespace chipcae::mesh;
using namespace chipcae::mesh::cut;
using chipcae::Index;
using chipcae::Real;
using sp5::require;
using Point = std::array<long double, 2>;
using Ring = std::vector<Point>;
constexpr std::size_t nx = 8, ny = 6, nz = 7, columns = nx * ny;
constexpr long double area_tolerance = 1e-12L;

struct Checks {
    std::size_t failures = 0;
    void expect(bool ok, const std::string& message) {
        if (!ok) {
            ++failures;
            std::cerr << "fvm-fields: " << message << '\n';
        }
    }
    template <class F>
    void run(const char* name, F test) {
        try {
            test();
        } catch (const std::exception& e) {
            expect(false, std::string(name) + ": " + e.what());
        }
    }
};

double threshold(const std::string& key) {
    std::ifstream in(std::string(CHIPCAE_SOURCE_DIR) + "/mesh/background/thresholds/k1.json");
    require(bool(in), "fvm-fields: threshold file opens");
    const std::string text((std::istreambuf_iterator<char>(in)), {});
    const auto pos = text.find('"' + key + '"');
    require(pos != std::string::npos, "fvm-fields: threshold key exists");
    const auto colon = text.find(':', pos);
    require(colon != std::string::npos, "fvm-fields: threshold value exists");
    return std::stod(text.substr(colon + 1));
}

StackModel stack(bool polygons) {
    StackModel s;
    s.id = "fvm-fields-contract";
    s.outline.box = {0, 0, .008, .006};
    for (std::size_t i = 0; i < 5; ++i) {
        Layer l;
        const bool copper = i % 2 == 0;
        l.name = copper ? "L" + std::to_string(i / 2 + 1) + "_cu" : "D" + std::to_string(i / 2 + 1);
        l.kind = copper ? "Patterned" : "Homogenized";
        l.material = "bt_eq";
        l.thickness = copper ? 35e-6 : 74.286e-6;
        // Keep adjacent z widths within the unchanged k1 max_ratio of two.
        l.n_elem_min = copper ? 1 : 2;
        l.grading = 1;
        if (copper) {
            CopperSpec c;
            c.metal = "cu";
            if (i == 2)
                c.fraction = .37;
            else if (polygons)
                c.polygons = CopperPolygons{"synthetic.polyset", i == 0 ? "L1" : "L3"};
            else
                c.fraction = 0;
            l.copper = c;
        }
        s.layers.push_back(l);
    }
    return s;
}

void append_ring(PolysetLayer& l, std::initializer_list<std::array<std::int64_t, 2>> points) {
    if (l.ring_offsets.empty())
        l.ring_offsets.push_back(0);
    for (auto p : points)
        l.coordinates.insert(l.coordinates.end(), p.begin(), p.end());
    l.ring_offsets.push_back(static_cast<std::int64_t>(l.coordinates.size() / 2));
}

Polyset polyset(bool corner) {
    Polyset p;
    p.dbu_m = 1e-9;
    PolysetLayer l1, l3;
    l1.name = "L1";
    l3.name = "L3";
    // Integer nanometres; preserve both overlapping input traces.
    append_ring(l1, {{500000, 1450000}, {4500000, 1450000}, {4500000, 1550000}, {500000, 1550000}});
    append_ring(l1,
                {{4100000, 1450000}, {7500000, 1450000}, {7500000, 1550000}, {4100000, 1550000}});
    append_ring(l1,
                {{1800000, 4200000}, {2300000, 3700000}, {2800000, 4200000}, {2300000, 4700000}});
    if (corner)
        append_ring(l1, {{0, 0}, {400000, 0}, {400000, 400000}, {0, 400000}});
    append_ring(l3, {{1300000, 700000}, {6700000, 700000}, {6700000, 5300000}, {1300000, 5300000}});
    append_ring(l3,
                {{3200000, 2200000}, {3200000, 3600000}, {4600000, 3600000}, {4600000, 2200000}});
    p.layers = {l1, l3};
    return p;
}

// Independent reference in metres. These rings have no overlaps: T1 union T2
// is constructed analytically; the hole keeps its negative signed area.
std::vector<Ring> reference(std::size_t copper, bool corner, bool empty) {
    if (empty || copper == 1)
        return {};
    std::vector<Ring> rings;
    if (copper == 0) {
        rings = {{{.5L, 1.45L}, {7.5L, 1.45L}, {7.5L, 1.55L}, {.5L, 1.55L}},
                 {{1.8L, 4.2L}, {2.3L, 3.7L}, {2.8L, 4.2L}, {2.3L, 4.7L}}};
        if (corner)
            rings.push_back({{0, 0}, {.4L, 0}, {.4L, .4L}, {0, .4L}});
    } else {
        rings = {{{1.3L, .7L}, {6.7L, .7L}, {6.7L, 5.3L}, {1.3L, 5.3L}},
                 {{3.2L, 2.2L}, {3.2L, 3.6L}, {4.6L, 3.6L}, {4.6L, 2.2L}}};
    }
    for (auto& ring : rings)
        for (auto& p : ring)
            for (auto& x : p)
                x *= 1e-3L;
    return rings;
}

long double signed_area(const Ring& ring) {
    long double sum = 0;
    for (std::size_t i = 0; i < ring.size(); ++i) {
        const auto& a = ring[i];
        const auto& b = ring[(i + 1) % ring.size()];
        sum += a[0] * b[1] - a[1] * b[0];
    }
    return sum / 2;
}

Ring clip(Ring ring, const std::array<long double, 4>& box) {
    // Four Sutherland-Hodgman half-space windows; no production raster/boolean calls.
    for (std::size_t side = 0; side < 4; ++side) {
        const auto axis = side % 2;
        const auto boundary = box[side];
        const auto inside = [&](Point p) {
            return side < 2 ? p[axis] >= boundary : p[axis] <= boundary;
        };
        Ring out;
        for (std::size_t i = 0; i < ring.size(); ++i) {
            const auto a = ring[i], b = ring[(i + 1) % ring.size()];
            if (inside(a))
                out.push_back(a);
            if (inside(a) != inside(b)) {
                const auto t = (boundary - a[axis]) / (b[axis] - a[axis]);
                Point p{a[0] + t * (b[0] - a[0]), a[1] + t * (b[1] - a[1])};
                p[axis] = boundary;
                out.push_back(p);
            }
        }
        ring = std::move(out);
    }
    return ring;
}

template <class T>
T checked(Expected<T> result) {
    if (!result)
        throw std::runtime_error("fvm-fields: " + result.error().path + ": " +
                                 result.error().reason);
    return std::move(result.value());
}

template <class T>
bool bits_equal(const std::vector<T>& a, const std::vector<T>& b) {
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::bit_cast<std::uint64_t>(a[i]) != std::bit_cast<std::uint64_t>(b[i]))
            return false;
    return true;
}

struct Cell {
    std::array<Real, 3> lo{}, hi{};
    std::array<std::size_t, 3> index{};
    long double area() const {
        return (static_cast<long double>(hi[0]) - lo[0]) *
               (static_cast<long double>(hi[1]) - lo[1]);
    }
};

Cell cell(const HexMeshArrays& m, std::size_t e) {
    require(e < m.attribute.size() && 8 * (e + 1) <= m.hex.size(),
            "fvm-fields: element connectivity in bounds");
    Cell c;
    c.lo.fill(std::numeric_limits<Real>::infinity());
    c.hi.fill(-std::numeric_limits<Real>::infinity());
    for (std::size_t n = 0; n < 8; ++n) {
        const auto v = m.hex[8 * e + n];
        require(v >= 0 && 3 * static_cast<std::size_t>(v) + 2 < m.vertices.size(),
                "fvm-fields: vertex index in bounds");
        for (std::size_t d = 0; d < 3; ++d) {
            const auto x = m.vertices[3 * static_cast<std::size_t>(v) + d];
            require(std::isfinite(x), "fvm-fields: finite vertex");
            c.lo[d] = std::min(c.lo[d], x);
            c.hi[d] = std::max(c.hi[d], x);
        }
    }
    for (std::size_t d = 0; d < 3; ++d) {
        const auto it = std::lower_bound(m.axes[d].begin(), m.axes[d].end(), c.lo[d]);
        require(it != m.axes[d].end(), "fvm-fields: lower vertex belongs to axis");
        c.index[d] = static_cast<std::size_t>(it - m.axes[d].begin());
        require(c.index[d] + 1 < m.axes[d].size() && std::abs(*it - c.lo[d]) <= 1e-15 &&
                    std::abs(m.axes[d][c.index[d] + 1] - c.hi[d]) <= 1e-15 && c.hi[d] > c.lo[d],
                "fvm-fields: vertices span one positive axis interval");
    }
    return c;
}

std::size_t copper_index(const HexMeshArrays& m, const StackModel& s, std::size_t e) {
    require(e < m.attribute.size(), "fvm-fields: attribute element in bounds");
    const auto a = m.attribute[e];
    require(a > 0 && static_cast<std::size_t>(a) <= m.attr_table.entries.size(),
            "fvm-fields: attribute record in bounds");
    const auto& name = m.attr_table.entries[static_cast<std::size_t>(a - 1)].layer;
    const auto it = std::find_if(
        s.layers.begin(), s.layers.end(), [&](const Layer& l) { return l.name == name; });
    require(it != s.layers.end(), "fvm-fields: attribute layer string resolves in stack");
    const auto li = static_cast<std::size_t>(it - s.layers.begin());
    require(li < m.copper_layer_of_layer.size(), "fvm-fields: layer mapping in bounds");
    const auto cl = m.copper_layer_of_layer[li];
    require(cl >= -1 && cl < 3, "fvm-fields: copper layer index in bounds");
    return cl < 0 ? 3 : static_cast<std::size_t>(cl);
}

void check_fields(Checks& checks,
                  const HexMeshArrays& m,
                  const StackModel& s,
                  bool corner,
                  bool polygons,
                  bool empty = false) {
    require(m.axes[0].size() == nx + 1 && m.axes[1].size() == ny + 1 && m.axes[2].size() == nz + 1,
            "fvm-fields: axes have 8 x 6 x 7 intervals");
    require(m.elem_gid.size() == columns * nz && m.attribute.size() == columns * nz &&
                m.partitioning.size() == columns * nz,
            "fvm-fields: 336 uncut elements and matching arrays");
    require(std::set<Index>(m.elem_gid.begin(), m.elem_gid.end()).size() == columns * nz,
            "fvm-fields: 336 unique element GIDs");
    require(m.copper_layer_of_layer == std::vector<Local>({0, -1, 1, -1, 2}) &&
                m.copper_phi.size() == 3 * columns,
            "fvm-fields: exact copper mapping and 144 fractions");
    struct Column {
        Cell geometry;
        std::set<std::size_t> vertical;
        std::set<Local> ranks;
        std::size_t count = 0;
    };
    std::map<std::size_t, Column> groups;
    std::array<long double, 3> max_error{}, area{};
    std::array<std::size_t, 3> coverage{};
    for (std::size_t e = 0; e < m.elem_gid.size(); ++e) {
        const auto gid = m.elem_gid[e];
        require(gid >= 0, "fvm-fields: nonnegative element GID");
        const auto col = static_cast<std::size_t>(gid) / nz;
        require(col < columns, "fvm-fields: GID column indexes copper field");
        const auto c = cell(m, e);
        auto& g = groups[col];
        if (g.count > 0)
            require(c.index[0] == g.geometry.index[0] && c.index[1] == g.geometry.index[1],
                    "fvm-fields: a GID column has one XY rectangle");
        g.geometry = c;
        ++g.count;
        g.vertical.insert(c.index[2]);
        g.ranks.insert(m.partitioning[e]);
        checks.expect(static_cast<std::size_t>(gid) % nz == c.index[2],
                      "GID vertical index matches axis interval");
        const auto cl = copper_index(m, s, e);
        if (cl == 3)
            continue;
        ++coverage[cl];
        const auto index = cl * columns + col;
        require(index < m.copper_phi.size(), "fvm-fields: copper value in bounds");
        const auto phi = m.copper_phi[index];
        checks.expect(std::isfinite(phi) && phi >= 0 && phi <= 1,
                      "finite copper fraction in [0,1]");
        long double expected = .37L;
        if (cl != 1) {
            long double clipped = 0;
            for (const auto& ring : reference(cl, corner, empty || !polygons))
                clipped += signed_area(clip(ring, {c.lo[0], c.lo[1], c.hi[0], c.hi[1]}));
            expected = clipped / c.area();
        } else {
            checks.expect(std::bit_cast<std::uint64_t>(phi) == std::bit_cast<std::uint64_t>(.37),
                          "constant fraction is bitwise 0.37");
        }
        max_error[cl] = std::max(max_error[cl], std::abs(phi - expected));
    }
    require(groups.size() == columns, "fvm-fields: all 48 columns represented");
    std::set<std::array<std::size_t, 2>> xy;
    for (const auto& [col, g] : groups) {
        checks.expect(g.count == nz && g.vertical.size() == nz && g.ranks.size() == 1,
                      "seven distinct vertical intervals stay on one rank");
        xy.insert({g.geometry.index[0], g.geometry.index[1]});
        for (std::size_t cl = 0; cl < 3; ++cl)
            area[cl] += m.copper_phi[cl * columns + col] * g.geometry.area();
    }
    checks.expect(xy.size() == columns, "GID columns cover 48 distinct XY rectangles");
    const std::array<long double, 3> expected_area{
        empty || !polygons ? 0 : (corner ? 1.36e-6L : 1.2e-6L),
        17.76e-6L,
        empty || !polygons ? 0 : 22.88e-6L};
    for (std::size_t cl = 0; cl < 3; ++cl) {
        const auto error = std::abs(area[cl] - expected_area[cl]);
        std::cout << "fvm-fields: " << (corner ? 'B' : 'A') << " copper=" << cl
                  << " max_phi_error=" << max_error[cl] << " area=" << area[cl]
                  << " conservation_error=" << error
                  << " relative=" << (expected_area[cl] > 0 ? error / expected_area[cl] : 0)
                  << " elements_checked=" << coverage[cl] << '\n';
        checks.expect(max_error[cl] <= 1e-12L,
                      "per-element copper fraction matches independent clip");
        checks.expect(error <=
                          (expected_area[cl] > 0 ? area_tolerance * expected_area[cl] : 1e-30L),
                      "copper area is conserved over the full bounding grid");
    }
}

Footprint chamfer() {
    return {{0, 0, .008, .006},
            {{.0006, 0},
             {.0074, 0},
             {.008, .0006},
             {.008, .0054},
             {.0074, .006},
             {.0006, .006},
             {0, .0054},
             {0, .0006}}};
}

CutGeometry manual_chamfer(const HexMeshArrays& m) {
    // Independently specified half-spaces: four box sides and four 0.6 mm chamfers.
    CutGeometry g;
    const Real n = 1 / std::sqrt(2.);
    for (const auto& [name, body] : m.attr_table.bodies) {
        (void)name;
        for (const auto& p : std::vector<Plane>{{body, {0, -1, 0}, 0},
                                                {body, {n, -n, 0}, .0074 * n},
                                                {body, {1, 0, 0}, .008},
                                                {body, {n, n, 0}, .0134 * n},
                                                {body, {0, 1, 0}, .006},
                                                {body, {-n, n, 0}, .0054 * n},
                                                {body, {-1, 0, 0}, 0},
                                                {body, {-n, -n, 0}, -.0006 * n}})
            g.planes.push_back(p);
    }
    return g;
}

void offsets(const std::vector<Local>& off, std::size_t rows, std::size_t values) {
    require(off.size() == rows + 1 && off.front() == 0 && std::is_sorted(off.begin(), off.end()) &&
                off.back() >= 0 && static_cast<std::size_t>(off.back()) == values,
            "fvm-fields: CSR has bounded monotone offsets and exact terminal count");
}

struct SurfaceAreas {
    std::array<long double, 3> cartesian{}, embedded{};
    std::array<long double, 6> openings{};
};

SurfaceAreas
surface_areas(const CutLayerArrays& cut, std::size_t row, const std::array<Real, 3>& normal) {
    // Surface rows belong to cut parents; sub_off separately partitions volume.
    offsets(cut.sq_off, cut.cut_elem.size(), cut.sq_w.size());
    SurfaceAreas areas;
    for (auto q = cut.sq_off[row]; q < cut.sq_off[row + 1]; ++q) {
        const auto qi = static_cast<std::size_t>(q);
        const auto side = cut.sq_side[qi];
        const auto w = cut.sq_w[qi];
        require(side >= -1 && side <= 5 && std::isfinite(w) && w > 0,
                "fvm-fields: valid side and positive physical area weight");
        if (side < 0) {
            for (std::size_t d = 0; d < 3; ++d)
                areas.embedded[d] += w * normal[d];
        } else {
            const auto side_index = static_cast<std::size_t>(side);
            areas.cartesian[side_index / 2] += (side % 2 == 0 ? -w : w);
            areas.openings[side_index] += w;
        }
    }
    return areas;
}

void check_surface_rows(Checks& checks) {
    // Synthetic parent-boundary data: unit boxes clipped at x=1/2 and y=1/4.
    // Partition the first solid into two equal subcells. The current convex
    // plane cutter cannot emit this layout; this exercises the array consumer.
    CutLayerArrays cut;
    cut.cut_elem = {0, 1};
    cut.sub_off = {0, 2, 3};
    cut.sub_volfrac = {.25, .25, .25};
    cut.sub_attribute = {1, 1, 1};
    cut.sq_off = {0, 6, 12};
    cut.sq_side = {0, -1, 2, 3, 4, 5, 0, 1, 2, -1, 4, 5};
    cut.sq_w = {1, 1, .5, .5, .5, .5, .25, .25, 1, 1, .25, .25};
    cut.exterior_area_vector = {{{-1, 0, 0}}, {{0, -1, 0}}};
    const std::array<std::array<Real, 3>, 2> normals{{{1, 0, 0}, {0, 1, 0}}};
    const std::array<std::array<Real, 6>, 2> expected_openings{
        {{1, 0, .5, .5, .5, .5}, {.25, .25, 1, 0, .25, .25}}};
    require(cut.cut_elem.size() != cut.sub_volfrac.size(),
            "fvm-fields: surface row fixture distinguishes parents from subcells");
    offsets(cut.sub_off, cut.cut_elem.size(), cut.sub_volfrac.size());
    for (std::size_t row = 0; row < cut.cut_elem.size(); ++row) {
        const auto areas = surface_areas(cut, row, normals[row]);
        for (std::size_t side = 0; side < 6; ++side)
            checks.expect(std::abs(areas.openings[side] - expected_openings[row][side]) <= 1e-12L,
                          "parent surface openings counted once regardless of subcell count");
        for (std::size_t d = 0; d < 3; ++d) {
            checks.expect(std::abs(areas.cartesian[d] - cut.exterior_area_vector[row][d]) <= 1e-12L,
                          "synthetic Cartesian area equals exterior");
            checks.expect(std::abs(areas.embedded[d] + cut.exterior_area_vector[row][d]) <= 1e-12L,
                          "synthetic embedded area equals negative exterior");
            checks.expect(std::abs(areas.cartesian[d] + areas.embedded[d]) <= 1e-12L,
                          "synthetic parent surface closes");
        }
    }
}

void check_cut(Checks& checks,
               const HexMeshArrays& original,
               const HexMeshArrays& m,
               const CutLayerArrays& cut,
               const CutLayerArrays& removal,
               const StackModel& s,
               Real volume_tolerance) {
    checks.expect(cut.cut_elem.size() == 28, "B has 28 cut parents (four corners times seven)");
    require(cut.cut_elem_gid.size() == cut.cut_elem.size() &&
                cut.exterior_area_vector.size() == cut.cut_elem.size(),
            "fvm-fields: cut parent fields have matching lengths");
    require(std::set<Local>(cut.cut_elem.begin(), cut.cut_elem.end()).size() == cut.cut_elem.size(),
            "fvm-fields: unique cut parents");
    require(std::set<Index>(cut.cut_elem_gid.begin(), cut.cut_elem_gid.end()).size() ==
                cut.cut_elem.size(),
            "fvm-fields: unique cut parent GIDs");
    offsets(cut.sub_off, cut.cut_elem.size(), cut.sub_volfrac.size());
    require(cut.sub_attribute.size() == cut.sub_volfrac.size(),
            "fvm-fields: subcell attributes match fractions");
    offsets(cut.sq_off, cut.cut_elem.size(), cut.sq_w.size());
    offsets(cut.vq_off, cut.sub_volfrac.size(), cut.vq_w.size());
    offsets(cut.bq_off, cut.sub_volfrac.size(), cut.bq_w.size());
    require(cut.sq_side.size() == cut.sq_w.size(), "fvm-fields: side records match weights");
    for (std::size_t d = 0; d < 3; ++d) {
        require(cut.sq_xi[d].size() == cut.sq_w.size() && cut.vq_xi[d].size() == cut.vq_w.size() &&
                    cut.bq_xi[d].size() == cut.bq_w.size() && cut.bq_n[d].size() == cut.bq_w.size(),
                "fvm-fields: quadrature coordinates and normals have matching lengths");
    }
    require(cut.eb_points.size() == 3 * cut.eb_parent_elem.size() &&
                cut.eb_xi.size() == cut.eb_points.size() &&
                cut.eb_normals.size() == cut.eb_points.size() && cut.eb_parent_elem.size() % 3 == 0,
            "fvm-fields: EB triangle fields have matching lengths");
    std::map<Local, std::array<Real, 3>> normal;
    for (std::size_t v = 0; v < cut.eb_parent_elem.size(); ++v) {
        const auto e = cut.eb_parent_elem[v];
        require(e >= 0 && static_cast<std::size_t>(e) < m.attribute.size(),
                "fvm-fields: EB parent index in bounds");
        const std::array<Real, 3> n{
            cut.eb_normals[3 * v], cut.eb_normals[3 * v + 1], cut.eb_normals[3 * v + 2]};
        const auto [it, inserted] = normal.emplace(e, n);
        Real norm = 0;
        for (std::size_t d = 0; d < 3; ++d) {
            require(std::isfinite(n[d]), "fvm-fields: finite EB normal");
            checks.expect(inserted || std::abs(n[d] - it->second[d]) <= 1e-12,
                          "all EB triangles on a chamfer parent have one normal");
            norm += n[d] * n[d];
        }
        checks.expect(std::abs(norm - 1) <= 1e-12, "unit EB normal");
        if (v % 3 != 0)
            checks.expect(e == cut.eb_parent_elem[v - 1], "EB triangle has one parent");
    }
    std::set<Index> retained(m.elem_gid.begin(), m.elem_gid.end());
    std::set<Index> void_gids;
    for (const auto e : removal.void_elem) {
        require(e >= 0 && static_cast<std::size_t>(e) < original.elem_gid.size(),
                "fvm-fields: original void index in bounds");
        const auto gid = original.elem_gid[static_cast<std::size_t>(e)];
        void_gids.insert(gid);
        checks.expect(!retained.contains(gid) &&
                          std::find(cut.cut_elem_gid.begin(), cut.cut_elem_gid.end(), gid) ==
                              cut.cut_elem_gid.end(),
                      "removed void GIDs absent from retained and cut elements");
    }
    std::map<std::array<std::size_t, 3>, Index> topology;
    for (std::size_t e = 0; e < m.elem_gid.size(); ++e)
        topology.emplace(cell(m, e).index, m.elem_gid[e]);
    long double max_closure = 0, max_cartesian = 0, max_embedded = 0;
    std::size_t truncated = 0, small_checked = 0, copper_checked = 0;
    for (std::size_t row = 0; row < cut.cut_elem.size(); ++row) {
        const auto e = cut.cut_elem[row];
        require(e >= 0 && static_cast<std::size_t>(e) < m.elem_gid.size(),
                "fvm-fields: cut parent in bounds");
        checks.expect(m.elem_gid[static_cast<std::size_t>(e)] == cut.cut_elem_gid[row] &&
                          std::find(original.elem_gid.begin(),
                                    original.elem_gid.end(),
                                    cut.cut_elem_gid[row]) != original.elem_gid.end(),
                      "cut parents preserve original GIDs");
        require(normal.contains(e), "fvm-fields: each cut parent has an EB normal");
        const auto c = cell(m, static_cast<std::size_t>(e));
        const Real hx = c.hi[0] - c.lo[0], hy = c.hi[1] - c.lo[1], hz = c.hi[2] - c.lo[2];
        const long double parent_area = 2.L * (hx * hy + hy * hz + hz * hx);
        const auto areas = surface_areas(cut, row, normal.at(e));
        const auto& cartesian = areas.cartesian;
        const auto& embedded = areas.embedded;
        const auto& openings = areas.openings;
        long double solid = 0;
        for (auto sub = cut.sub_off[row]; sub < cut.sub_off[row + 1]; ++sub) {
            const auto si = static_cast<std::size_t>(sub);
            const auto vf = cut.sub_volfrac[si];
            checks.expect(std::isfinite(vf) && vf > 0, "finite positive subcell fraction");
            solid += vf;
            const auto a = cut.sub_attribute[si];
            require(a > 0 && static_cast<std::size_t>(a) <= m.attr_table.entries.size(),
                    "fvm-fields: subcell attribute in bounds");
            if (std::find(cut.small_subs.begin(), cut.small_subs.end(), sub) !=
                cut.small_subs.end()) {
                ++small_checked;
                bool neighbor = false;
                for (std::size_t side = 0; side < 6; ++side) {
                    if (openings[side] <= 0)
                        continue;
                    auto target = c.index;
                    const auto d = side / 2;
                    if (side % 2 == 0) {
                        if (target[d] == 0)
                            continue;
                        --target[d];
                    } else {
                        ++target[d];
                    }
                    const auto it = topology.find(target);
                    neighbor =
                        neighbor || (it != topology.end() && !void_gids.contains(it->second));
                }
                checks.expect(neighbor,
                              "small subcell has positive opening to an existing solid neighbor");
            }
        }
        checks.expect(std::isfinite(solid) && solid > 0 && solid <= 1 + 1e-14L,
                      "parent sum of subcell fractions is positive and at most one");
        const auto cl = copper_index(m, s, static_cast<std::size_t>(e));
        if (solid > 0 && cl < 3) {
            const auto col = static_cast<std::size_t>(m.elem_gid[static_cast<std::size_t>(e)]) / nz;
            require(col < columns && cl * columns + col < m.copper_phi.size(),
                    "fvm-fields: cut copper index in bounds");
            const auto ratio = m.copper_phi[cl * columns + col] / solid;
            const auto fraction = std::min(1.L, ratio);
            checks.expect(std::isfinite(fraction) && fraction >= 0 && fraction <= 1,
                          "min(1,phi/s) is finite and in [0,1]");
            truncated += ratio > 1 ? 1 : 0;
            ++copper_checked;
        }
        for (std::size_t d = 0; d < 3; ++d) {
            const auto exterior = cut.exterior_area_vector[row][d];
            require(std::isfinite(exterior), "fvm-fields: finite exterior area vector");
            const auto error = std::abs(cartesian[d] + embedded[d]);
            max_closure = std::max(max_closure, error / parent_area);
            max_cartesian =
                std::max(max_cartesian, std::abs(cartesian[d] - exterior) / parent_area);
            max_embedded = std::max(max_embedded, std::abs(embedded[d] + exterior) / parent_area);
        }
    }
    for (const auto sub : cut.small_subs)
        require(sub >= 0 && static_cast<std::size_t>(sub) < cut.sub_volfrac.size(),
                "fvm-fields: small_subs index in bounds");
    checks.expect(small_checked == cut.small_subs.size(), "every small_subs entry checked");
    const auto stats = checked(cut_stats(m, cut));
    constexpr long double expected_volume = 47.28e-6L * 253.572e-6L;
    const auto volume_error = std::abs(stats.solid_volume - expected_volume) / expected_volume;
    std::cout << "fvm-fields: B cut_count=" << cut.cut_elem.size()
              << " volume_relative=" << volume_error << " closure_relative=" << max_closure
              << " C_minus_exterior_relative=" << max_cartesian
              << " E_plus_exterior_relative=" << max_embedded << " small_checked=" << small_checked
              << " removed_void_checked=" << void_gids.size()
              << " cut_copper_checked=" << copper_checked << " phi_over_s_gt_one=" << truncated
              << '\n';
    if (small_checked == 0)
        std::cout << "fvm-fields: small-subcell merge-target coverage is vacuous\n";
    if (void_gids.empty())
        std::cout << "fvm-fields: void-removal coverage is vacuous\n";
    checks.expect(volume_error <= volume_tolerance,
                  "cut_stats volume matches analytic octagonal prism");
    checks.expect(max_cartesian <= 1e-12L, "Cartesian area vector equals exterior_area_vector");
    checks.expect(max_embedded <= 1e-12L,
                  "embedded area vector equals negative exterior_area_vector");
    checks.expect(max_closure <= 1e-12L, "cut closure sum(Cartesian + embedded) equals zero");
}

void check_reference(Checks& checks) {
    for (const bool corner : {false, true})
        for (const std::size_t cl : {std::size_t{0}, std::size_t{2}}) {
            long double area = 0;
            for (const auto& ring : reference(cl, corner, false))
                area += signed_area(ring);
            const auto expected = cl == 2 ? 22.88e-6L : (corner ? 1.36e-6L : 1.2e-6L);
            checks.expect(std::abs(area - expected) <= area_tolerance * expected,
                          "independent reference area self-check");
        }
}
} // namespace

int main() {
    return sp5::run([] {
        std::cout << std::setprecision(17);
        Checks checks;
        check_reference(checks);
        checks.run("parent surface rows", [&] { check_surface_rows(checks); });
        GridOptions grid;
        grid.nx = static_cast<Local>(nx);
        grid.ny = static_cast<Local>(ny);
        grid.levels = 1;
        grid.cell_budget = static_cast<Index>(threshold("cell_budget"));
        grid.snap_tol = threshold("snap_tol");
        grid.max_ratio = threshold("max_ratio");
        CutOptions options;
        options.quad_degree = 6;
        options.keep_void = false;
        options.export_polyhedra = true;
        options.snap_phi = threshold("snap_phi");
        options.small_sub_vf = threshold("small_sub_vf");
        const auto volume_tolerance = threshold("cut_volume_relative");
        auto s = stack(true);
        auto probe = build_tensor_grid(s, grid, polyset(false));
        const bool no_ecad =
            !probe && probe.error().path == "copper" &&
            probe.error().reason == "polygon copper rasterization requires CHIPCAE_ENABLE_ECAD";
        if (no_ecad) {
            std::cout << "fvm-fields: polygon raster skipped (no ECAD)\n";
            s = stack(false);
        } else {
            (void)checked(std::move(probe));
        }
        const auto build = [&](const StackModel& input, bool corner, Local ranks = 1) {
            auto bounding = input;
            bounding.outline.polygon.clear();
            auto opt = grid;
            opt.ranks = ranks;
            return checked(build_tensor_grid(bounding, opt, polyset(corner)));
        };
        checks.run("A", [&] {
            const auto a = build(s, false);
            check_fields(checks, a, s, false, !no_ecad);
            const auto parallel = build(s, false, 3);
            check_fields(checks, parallel, s, false, !no_ecad);
            checks.expect(parallel.elem_gid == a.elem_gid &&
                              bits_equal(parallel.copper_phi, a.copper_phi),
                          "ranks=3 preserves GIDs and bitwise copper fractions");
            checks.expect(bits_equal(build(s, false).copper_phi, a.copper_phi),
                          "A copper fractions are bitwise deterministic");
        });
        checks.run("B", [&] {
            auto b = s;
            b.outline = chamfer();
            const auto bounding = build(b, true);
            check_fields(checks, bounding, b, true, !no_ecad);
            const auto geometry = manual_chamfer(bounding);
            const auto removal = checked(build_cut_layer(bounding, geometry, options));
            auto manual_mesh = bounding;
            const auto manual = checked(build_cut_layer(manual_mesh, geometry, options));
            auto mesh = bounding;
            auto wrapped = checked(apply_outline_cut(mesh, b.outline, options));
            require(wrapped.has_value(), "fvm-fields: B outline cut result is engaged");
            const auto& cut = *wrapped;
            require(
                manual.cut_elem_gid == cut.cut_elem_gid && manual.sub_off == cut.sub_off &&
                    manual.sub_volfrac.size() == cut.sub_volfrac.size(),
                "fvm-fields: direct and outline cut routes have identical parent/subcell layout");
            checks.expect(manual_mesh.elem_gid == mesh.elem_gid && manual_mesh.hex == mesh.hex &&
                              manual_mesh.attribute == mesh.attribute,
                          "direct and outline routes retain identical mesh integer arrays");
            for (std::size_t i = 0; i < cut.sub_volfrac.size(); ++i)
                checks.expect(std::abs(cut.sub_volfrac[i] - manual.sub_volfrac[i]) <= 1e-12,
                              "independent half-spaces match outline subcell fractions");
            check_fields(checks, mesh, b, true, !no_ecad);
            check_cut(checks, bounding, mesh, cut, removal, b, volume_tolerance);
            auto repeated_mesh = build(b, true);
            auto repeated = checked(apply_outline_cut(repeated_mesh, b.outline, options));
            require(repeated.has_value(), "fvm-fields: repeated B cut result is engaged");
            checks.expect(bits_equal(mesh.copper_phi, repeated_mesh.copper_phi) &&
                              bits_equal(cut.sub_volfrac, repeated->sub_volfrac),
                          "B copper fractions and subcell fractions are bitwise deterministic");
        });
        checks.run("C", [&] {
            const auto bounding = build(s, false);
            for (const auto& ring : std::vector<std::vector<std::array<Real, 2>>>{
                     {{0, 0}, {.008, 0}, {.008, .006}, {0, .006}},
                     {{0, 0}, {0, .006}, {.008, .006}, {.008, 0}, {0, 0}}}) {
                auto outline = s.outline;
                outline.polygon = ring;
                auto mesh = bounding;
                const auto result = checked(apply_outline_cut(mesh, outline, options));
                checks.expect(result.has_value() && result->cut_elem.empty(),
                              "rectangle spelling produces engaged result with no cut parents");
                checks.expect(mesh.hex == bounding.hex, "rectangle spelling preserves hex exactly");
                checks.expect(mesh.elem_gid == bounding.elem_gid,
                              "rectangle spelling preserves elem_gid exactly");
                checks.expect(mesh.attribute == bounding.attribute,
                              "rectangle spelling preserves attribute exactly");
                checks.expect(bits_equal(mesh.vertices, bounding.vertices),
                              "rectangle spelling preserves vertices bitwise");
            }
            std::cout << "fvm-fields: rectangle_spellings_checked=2\n";
        });
        checks.run("case-sensitive layer name", [&] {
            auto p = polyset(false);
            p.layers[0].name = "l1";
            const auto result = build_tensor_grid(stack(true), grid, p);
            checks.expect(!result && result.error().path == "copper" &&
                              result.error().reason == "polyset does not contain layer L1",
                          "lowercase l1 does not match L1 and returns exact copper error");
        });
        if (!no_ecad)
            checks.run("empty polygon layers", [&] {
                auto p = polyset(false);
                for (auto& layer : p.layers) {
                    layer.ring_offsets = {0, 0};
                    layer.coordinates.clear();
                }
                const auto mesh = checked(build_tensor_grid(s, grid, p));
                check_fields(checks, mesh, s, false, true, true);
            });
        std::cout << "fvm-fields: assertion_failures=" << checks.failures << '\n';
        require(checks.failures == 0, "fvm-fields: acceptance contract assertions failed");
    });
}
