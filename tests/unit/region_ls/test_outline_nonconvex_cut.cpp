#include "chipcae/mesh/cut/cut_stats.hpp"
#include "chipcae/mesh/cut/detail/outline_nonconvex.hpp"
#include "chipcae/mesh/cut/outline_cut.hpp"
#include "support.hpp"

#include <bit>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iomanip>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <string_view>
#include <tuple>
#include <type_traits>

namespace {
using namespace chipcae;
using namespace chipcae::mesh;
using namespace chipcae::mesh::cut;
namespace support = sp5;
namespace general = chipcae::mesh::cut::detail;
using support::require;
using P2 = std::array<Real, 2>;
using P3 = std::array<Real, 3>;
using Ring = std::vector<P2>;

void check(bool ok, const std::string& message) {
    if (!ok)
        throw std::runtime_error(message);
}
void near(Real actual, Real expected, Real rel, std::string_view context) {
    std::ostringstream message;
    message << std::setprecision(17) << context << ": actual=" << actual
            << ", expected=" << expected;
    check(std::isfinite(actual) && std::abs(actual / expected - 1) <= rel, message.str());
}
template <class T>
T value(Expected<T> result, std::string_view context) {
    if (!result)
        throw std::runtime_error(std::string(context) + ": expected success; got " +
                                 result.error().path + ": " + result.error().reason);
    return std::move(result.value());
}
P3 minus(P3 a, P3 b) {
    for (int d = 0; d < 3; ++d)
        a[d] -= b[d];
    return a;
}
P3 cross(P3 a, P3 b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
Real dot(P3 a, P3 b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
Real norm(P3 a) {
    return std::sqrt(dot(a, a));
}
P3 point(const std::vector<Real>& xyz, std::size_t i) {
    return {xyz.at(3 * i), xyz.at(3 * i + 1), xyz.at(3 * i + 2)};
}

// Recursive field comparison checks exact values and floating representations,
// including signed zero, without struct padding or pointer identities.
struct Bits {
    std::vector<std::uint64_t> words;
    std::vector<Real> values;
    template <class T>
        requires std::is_arithmetic_v<T>
    void add(T v) {
        if constexpr (std::is_floating_point_v<T>) {
            values.push_back(v);
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
    check(x.values == y.values && x.words == y.words,
          std::string(context) + ": recursive bit patterns differ");
}
StackModel stack() {
    StackModel s;
    s.id = "outline-nonconvex-synthetic";
    s.unit = "m";
    s.outline.box = {0, 0, .02, .02};
    Layer l;
    l.name = "substrate";
    l.kind = "Homogenized";
    l.material = "organic_eq";
    l.thickness = .001;
    l.n_elem_min = 2;
    l.grading = 1;
    s.layers = {l};
    return s;
}
Real distance(P2 p, P2 a, P2 b) {
    const Real dx = b[0] - a[0], dy = b[1] - a[1];
    const Real t = std::clamp(
        ((p[0] - a[0]) * dx + (p[1] - a[1]) * dy) / (dx * dx + dy * dy), Real(0), Real(1));
    return std::hypot(p[0] - a[0] - t * dx, p[1] - a[1] - t * dy);
}
// Independent minimum point-to-segment distance and ray parity, not max(planes).
Real signed_distance(P2 p, const Ring& ring) {
    Real best = std::numeric_limits<Real>::infinity();
    bool inside = false;
    for (std::size_t i = 0; i < ring.size(); ++i) {
        const auto a = ring[i], b = ring[(i + 1) % ring.size()];
        best = std::min(best, distance(p, a, b));
        if ((a[1] > p[1]) != (b[1] > p[1]) &&
            p[0] < a[0] + (p[1] - a[1]) * (b[0] - a[0]) / (b[1] - a[1]))
            inside = !inside;
    }
    return inside ? -best : best;
}
bool on_edge(P2 p, P2 a, P2 b) {
    return distance(p, a, b) < 1e-14;
}
bool box_edge(P2 a, P2 b) {
    for (int d = 0; d < 2; ++d)
        for (Real x : {0., .02})
            if (std::abs(a[d] - x) < 1e-14 && std::abs(b[d] - x) < 1e-14)
                return true;
    return false;
}
struct Shape {
    Ring outline;
    Real volume, embedded_area;
};
Shape l_shape(Real x = .0113, Real y = .0137) {
    return {{{0, 0}, {.02, 0}, {.02, y}, {x, y}, {x, .02}, {0, .02}}, 3.4519e-7, 1.5e-5};
}
Shape l0_shape() {
    auto s = l_shape(.0125, .0125);
    s.volume = 3.4375e-7;
    s.embedded_area = 0;
    return s;
}
Shape u_shape() {
    return {{{0, 0},
             {.02, 0},
             {.02, .02},
             {.0129, .02},
             {.0129, .0123},
             {.0071, .0123},
             {.0071, .02},
             {0, .02}},
            3.5534e-7,
            2.12e-5};
}
Shape v_shape() {
    return {{{0, 0}, {.0064, 0}, {.0103, .0087}, {.0142, 0}, {.02, 0}, {.02, .02}, {0, .02}},
            3.6607e-7,
            2 * std::hypot(.0039, .0087) * .001};
}
Shape r_shape() {
    return {{{0, 0},
             {.0086, 0},
             {.0086, .0043},
             {.0043, .0043},
             {.0043, .0157},
             {.0157, .0157},
             {.0157, .0043},
             {.0117, .0043},
             {.0117, 0},
             {.02, 0},
             {.02, .02},
             {0, .02}},
            2.5671e-7,
            5.11e-5};
}
Footprint footprint(const Ring& ring) {
    return {{0, 0, .02, .02}, ring};
}
HexMeshArrays background(const Ring& ring, int ranks = 1) {
    auto s = stack();
    s.outline.polygon = ring;
    s.outline.polygon.clear();
    auto m = support::grid(s, 8, ranks);
    require(m.elem_gid.size() == 128 && m.node_gid.size() == 243,
            "S background must contain 128 elements and 243 nodes");
    require(m.axes[0].size() == 9 && m.axes[1].size() == 9 && m.axes[2].size() == 3,
            "S background must have 8 x 8 x 2 intervals");
    return m;
}
struct Output {
    HexMeshArrays mesh;
    CutLayerArrays cut;
};
Output build(const Shape& s, CutOptions o = {}, int ranks = 1) {
    auto m = background(s.outline, ranks);
    auto c = value(general::apply_outline_cut_general(m, footprint(s.outline), o, {}),
                   "apply_outline_cut_general");
    require(c.has_value(), "nonempty outline must return a cut layer");
    return {std::move(m), std::move(*c)};
}
std::pair<P3, P3> bounds(const HexMeshArrays& m, std::size_t e) {
    auto lo = point(m.vertices, m.hex.at(8 * e));
    return {lo, minus(point(m.vertices, m.hex.at(8 * e + 6)), lo)};
}
P3 physical(const HexMeshArrays& m, std::size_t e, P3 xi) {
    const auto [lo, size] = bounds(m, e);
    for (int d = 0; d < 3; ++d)
        xi[d] = lo[d] + xi[d] * size[d];
    return xi;
}
P3 reference(const std::array<std::vector<Real>, 3>& a, std::size_t i) {
    return {a[0].at(i), a[1].at(i), a[2].at(i)};
}

// Every embedded point/normal must be attributable to one exterior outline edge.
// This rejects diagonal caps even if aggregate areas happen to cancel.
void exterior_point(P3 p, const Ring& outline, const std::optional<P3>& normal = {}) {
    bool found = false;
    for (std::size_t i = 0; i < outline.size(); ++i) {
        auto a = outline[i], b = outline[(i + 1) % outline.size()];
        if (box_edge(a, b) || !on_edge({p[0], p[1]}, a, b))
            continue;
        P3 n{b[1] - a[1], a[0] - b[0], 0};
        Real length = norm(n);
        for (auto& x : n)
            x /= length;
        bool matches = true;
        if (normal)
            for (int d = 0; d < 3; ++d)
                matches &= std::abs((*normal)[d] - n[d]) <= 1e-12;
        found |= matches;
    }
    require(found, "embedded point/normal must lie on an original non-box exterior edge");
}
void valid_output(const Output& out, bool require_cut = true) {
    const auto& m = out.mesh;
    const auto& c = out.cut;
    if (require_cut)
        require(!c.cut_elem.empty(), "fixture must produce cut cells");
    require(c.sub_off.size() == c.cut_elem.size() + 1, "sub CSR covers cut cells");
    require(c.polyhedra.has_value(), "requested polyhedra must be present");
    const auto& p = *c.polyhedra;
    require(p.parent_elem_gid.size() == c.cut_elem.size(), "one polyhedron per cut cell");
    require(p.poly_off.size() == c.cut_elem.size() + 1, "poly CSR covers cut cells");
    require(p.face_off.back() == static_cast<Index>(p.face_conn.size()), "face CSR terminal");
    require(p.poly_off.back() == static_cast<Index>(p.poly_faces.size()), "poly CSR terminal");
    require(c.vq_off.back() == static_cast<Local>(c.vq_w.size()), "vq CSR terminal");
    require(c.bq_off.back() == static_cast<Local>(c.bq_w.size()), "bq CSR terminal");
    require(c.sq_off.back() == static_cast<Local>(c.sq_w.size()), "sq CSR terminal");
    std::set<Local> unique(c.cut_elem.begin(), c.cut_elem.end());
    require(unique.size() == c.cut_elem.size(), "cut cells appear once");
    for (std::size_t row = 0; row < c.cut_elem.size(); ++row) {
        const auto e = static_cast<std::size_t>(c.cut_elem[row]);
        const auto sub = static_cast<std::size_t>(c.sub_off[row]);
        require(c.sub_off[row + 1] == static_cast<Local>(sub + 1), "one subcell per cut cell");
        require(c.sub_volfrac.at(sub) > 0, "positive subcell volume");
        require(c.sub_attribute.at(sub) == m.attribute.at(e), "subcell attribute is remapped");
        Real weight = 0;
        for (Local q = c.vq_off.at(sub); q < c.vq_off.at(sub + 1); ++q)
            weight += c.vq_w.at(q);
        near(weight, c.sub_volfrac[sub], 1e-10, "vq weight sum equals subcell fraction");
        const auto it =
            std::find(p.parent_elem_gid.begin(), p.parent_elem_gid.end(), m.elem_gid[e]);
        require(it != p.parent_elem_gid.end(), "polyhedron parent gid");
        const auto poly = static_cast<std::size_t>(it - p.parent_elem_gid.begin());
        const auto [lo, size] = bounds(m, e);
        const Real jac = size[0] * size[1] * size[2];
        const Real max_face = std::max({size[0] * size[1], size[0] * size[2], size[1] * size[2]});
        std::map<std::pair<Index, Index>, std::array<int, 2>> edges;
        P3 residual{};
        Real signed_volume = 0;
        for (Index f = p.poly_off.at(poly); f < p.poly_off.at(poly + 1); ++f) {
            const auto face = p.poly_faces.at(f);
            const auto begin = p.face_off.at(face), end = p.face_off.at(face + 1);
            require(end - begin >= 3, "nondegenerate exported ring");
            const auto a = point(p.points, p.face_conn.at(begin));
            std::set<Index> vertices;
            for (Index j = begin; j < end; ++j) {
                auto u = p.face_conn.at(j), v = p.face_conn.at(j + 1 == end ? begin : j + 1);
                require(u != v, "no duplicate consecutive/closing exported vertices");
                vertices.insert(u);
                auto key = std::minmax(u, v);
                auto& counts = edges[{key.first, key.second}];
                ++counts[u < v ? 0 : 1];
                require(p.point_parent_elem.at(u) == static_cast<Local>(e),
                        "point parent remapped");
                const auto xyz = point(p.points, u), ref = point(p.point_xi, u);
                auto recovered = physical(m, e, ref);
                for (int d = 0; d < 3; ++d)
                    require(std::abs(xyz[d] - recovered[d]) <= 1e-14, "export point reference");
            }
            require(vertices.size() == static_cast<std::size_t>(end - begin),
                    "distinct ring vertices");
            P3 av{};
            for (Index j = begin + 1; j + 1 < end; ++j) {
                auto b = point(p.points, p.face_conn.at(j));
                auto d = point(p.points, p.face_conn.at(j + 1));
                auto v = cross(minus(b, a), minus(d, a));
                for (int axis = 0; axis < 3; ++axis)
                    av[axis] += v[axis] / 2;
                signed_volume += dot(minus(a, lo), cross(minus(b, lo), minus(d, lo))) / 6;
            }
            require(norm(av) > 0, "no zero-area exported face");
            for (int d = 0; d < 3; ++d)
                residual[d] += av[d];
        }
        for (const auto& [edge, counts] : edges) {
            (void)edge;
            require(counts[0] == 1 && counts[1] == 1,
                    "each undirected exported edge occurs twice in opposite directions");
        }
        for (Real r : residual)
            require(std::abs(r) <= 1e-12 * max_face, "area-normal closure residual");
        require(signed_volume > 0, "positive signed nonconvex boundary volume");
        near(signed_volume, jac * c.sub_volfrac[sub], 1e-10, "boundary and ordered piece volume");
        near(p.volume.at(poly), signed_volume, 1e-10, "export volume");
        exact(p.volfrac_ref.at(poly), c.sub_volfrac[sub], "export authoritative fraction");
    }
    sp5::compact(m);
}
void surfaces(const Output& out, const Shape& s) {
    const auto& m = out.mesh;
    const auto& c = out.cut;
    Real sq = 0, bq = 0, eb = 0;
    for (std::size_t row = 0; row < c.cut_elem.size(); ++row) {
        auto e = static_cast<std::size_t>(c.cut_elem[row]);
        for (Local q = c.sq_off.at(row); q < c.sq_off.at(row + 1); ++q) {
            require(c.sq_w.at(q) > 0, "positive surface quadrature weight");
            if (c.sq_side.at(q) == -1) {
                sq += c.sq_w[q];
                exterior_point(physical(m, e, reference(c.sq_xi, q)), s.outline);
            }
        }
        P3 from_sq{};
        for (Local q = c.sq_off.at(row); q < c.sq_off.at(row + 1); ++q) {
            const auto side = c.sq_side.at(q);
            require(side >= -1 && side <= 5, "valid parent-face side");
            if (side >= 0)
                from_sq[side / 2] += (side % 2 == 0 ? -1 : 1) * c.sq_w[q];
        }
        P3 total = c.exterior_area_vector.at(row);
        const auto sub = static_cast<std::size_t>(c.sub_off.at(row));
        for (Local q = c.bq_off.at(sub); q < c.bq_off.at(sub + 1); ++q) {
            bq += c.bq_w.at(q);
            const auto n = reference(c.bq_n, q);
            exterior_point(physical(m, e, reference(c.bq_xi, q)), s.outline, n);
            for (int d = 0; d < 3; ++d)
                total[d] += n[d] * c.bq_w[q];
            for (int d = 0; d < 3; ++d)
                from_sq[d] += n[d] * c.bq_w[q];
        }
        const auto [lo, size] = bounds(m, e);
        (void)lo;
        const Real max_face = std::max({size[0] * size[1], size[0] * size[2], size[1] * size[2]});
        for (Real r : total)
            require(std::abs(r) <= 1e-12 * max_face, "surface/exterior area vector closure");
        for (Real r : from_sq)
            require(std::abs(r) <= 1e-12 * max_face, "quadrature parent/embedded closure");
    }
    require(c.eb_points.size() % 9 == 0 && c.eb_normals.size() == c.eb_points.size() &&
                c.eb_xi.size() == c.eb_points.size() &&
                c.eb_parent_elem.size() * 3 == c.eb_points.size(),
            "EB triangles and parallel arrays");
    for (std::size_t t = 0; t < c.eb_points.size() / 9; ++t) {
        P3 a = point(c.eb_points, 3 * t), b = point(c.eb_points, 3 * t + 1),
           d = point(c.eb_points, 3 * t + 2);
        auto av = cross(minus(b, a), minus(d, a));
        require(norm(av) > 0, "no degenerate EB triangles");
        eb += norm(av) / 2;
        auto n = point(c.eb_normals, 3 * t);
        require(dot(av, n) > 0, "EB winding is outward");
        for (int axis = 0; axis < 3; ++axis)
            require(std::abs(av[axis] / norm(av) - n[axis]) <= 1e-12,
                    "EB geometric normal matches reported normal");
        P3 center{};
        for (int axis = 0; axis < 3; ++axis)
            center[axis] = (a[axis] + b[axis] + d[axis]) / 3;
        exterior_point(center, s.outline, n);
        for (int j = 0; j < 3; ++j) {
            exact(point(c.eb_normals, 3 * t + j), n, "constant EB triangle normal");
            const auto e = c.eb_parent_elem.at(3 * t + j);
            auto recovered = physical(m, e, point(c.eb_xi, 3 * t + j));
            auto xyz = point(c.eb_points, 3 * t + j);
            exterior_point(xyz, s.outline, n);
            for (int axis = 0; axis < 3; ++axis)
                require(std::abs(recovered[axis] - xyz[axis]) <= 1e-14, "EB reference mapping");
        }
    }
    const auto& p = *c.polyhedra;
    for (std::size_t poly = 0; poly < p.parent_elem_gid.size(); ++poly)
        for (Index f = p.poly_off.at(poly); f < p.poly_off.at(poly + 1); ++f) {
            const auto face = p.poly_faces.at(f);
            const auto begin = p.face_off.at(face), end = p.face_off.at(face + 1);
            bool parent_face = false;
            for (int axis = 0; axis < 3; ++axis)
                for (Real side : {0., 1.}) {
                    bool on_parent = true;
                    for (Index j = begin; j < end; ++j)
                        on_parent &=
                            std::abs(point(p.point_xi, p.face_conn.at(j))[axis] - side) <= 1e-12;
                    parent_face |= on_parent;
                }
            if (parent_face)
                continue;
            P3 av{}, center{};
            const auto a = point(p.points, p.face_conn.at(begin));
            for (Index j = begin + 1; j + 1 < end; ++j) {
                const auto triangle = cross(minus(point(p.points, p.face_conn.at(j)), a),
                                            minus(point(p.points, p.face_conn.at(j + 1)), a));
                for (int d = 0; d < 3; ++d)
                    av[d] += triangle[d] / 2;
            }
            const auto length = norm(av);
            require(length > 0, "exported embedded face has positive area");
            for (auto& d : av)
                d /= length;
            for (Index j = begin; j < end; ++j) {
                const auto xyz = point(p.points, p.face_conn.at(j));
                exterior_point(xyz, s.outline, av);
                for (int d = 0; d < 3; ++d)
                    center[d] += xyz[d] / static_cast<Real>(end - begin);
            }
            exterior_point(center, s.outline, av);
        }
    if (s.embedded_area > 0) {
        near(sq, s.embedded_area, 1e-10, "embedded sq area");
        near(bq, s.embedded_area, 1e-10, "bq area");
        near(eb, s.embedded_area, 1e-10, "EB area");
    } else {
        require(std::abs(sq) <= 1.5e-17 && std::abs(bq) <= 1.5e-17 && std::abs(eb) <= 1.5e-17,
                "L0 embedded area must vanish");
    }
    std::cout << std::setprecision(17) << "embedded area=" << sq << " expected=" << s.embedded_area
              << " absolute_error=" << std::abs(sq - s.embedded_area) << '\n';
}
void level_sets(const Output& out, const Ring& outline, const CutGeometry& extra = {}) {
    const auto& m = out.mesh;
    const auto& levels = out.cut.level_sets;
    require(levels.size() == m.attr_table.bodies.size(), "one level set per body registry entry");
    std::size_t k = 0;
    for (const auto& [name, body] : m.attr_table.bodies) {
        (void)name;
        const auto& ls = levels.at(k++);
        require(ls.body == body && ls.phi.size() == m.node_gid.size(), "level set body/order/size");
        for (std::size_t v = 0; v < m.node_gid.size(); ++v) {
            auto xyz = point(m.vertices, v);
            Real expected = signed_distance({xyz[0], xyz[1]}, outline);
            for (const auto& p : extra.planes)
                if (p.body == 0 || p.body == body) {
                    Real length = norm(p.n);
                    P3 n = p.n;
                    for (auto& x : n)
                        x *= 1 / length;
                    expected = std::max(expected, dot(n, xyz) - p.d / length);
                }
            require(std::isfinite(ls.phi[v]) && std::abs(ls.phi[v] - expected) <= 1e-14,
                    "level set must measure the exterior ring, not decomposition caps");
        }
    }
}
void volume_case(const Shape& s) {
    auto out = build(s);
    const auto stats = value(cut_stats(out.mesh, out.cut), "cut_stats");
    std::cout << std::setprecision(17) << "solid volume=" << stats.solid_volume
              << " expected=" << s.volume
              << " relative_error=" << std::abs(stats.solid_volume - s.volume) / s.volume << '\n';
    near(stats.solid_volume, s.volume, 1e-10, "analytic outline volume");
    valid_output(out, s.embedded_area > 0);
    surfaces(out, s);
    level_sets(out, s.outline);
}
void extra_planes_two_bodies() {
    auto s = stack();
    auto cap = s.layers.front();
    cap.name = "cap";
    s.layers.push_back(cap);
    const auto original = support::grid(s, 8);
    const auto substrate = original.attr_table.bodies.at("substrate");
    const auto upper = original.attr_table.bodies.at("cap");
    require(substrate != upper, "two layers must have distinct bodies");
    const auto shape = l_shape();
    for (const auto clipped_body : {substrate, upper}) {
        auto m = original;
        // x <= .0151 leaves area .0113*.02 + (.0151-.0113)*.0137
        // in the selected body; the other body retains the entire L outline.
        const CutGeometry extra{{Plane{clipped_body, {2, 0, 0}, .0302}}};
        auto cut = value(general::apply_outline_cut_general(m, footprint(shape.outline), {}, extra),
                         "nonconvex outline with body-specific extra plane");
        require(cut.has_value(), "two-body outline must return a cut layer");
        const auto stats = value(cut_stats(m, *cut), "two-body cut_stats");
        for (const auto body : {substrate, upper})
            near(stats.body_volume.at(body),
                 body == clipped_body ? 2.7806e-7 : shape.volume,
                 1e-10,
                 "extra plane applies only to its body; outline applies to both");
        near(stats.solid_volume, 6.2325e-7, 1e-10, "two-body analytic union volume");
        Output out{std::move(m), std::move(*cut)};
        valid_output(out, true);
        level_sets(out, shape.outline, extra);
    }
}
void grid_aligned_L0() {
    auto original = background(l0_shape().outline);
    auto out = build(l0_shape());
    require(out.mesh.elem_gid.size() == 110, "L0 retains exactly 110 elements");
    // Mutable keep_void=false returns compacted indices. The 18 voids are
    // removed original GIDs; returning stale void_elem indices is invalid.
    require(out.cut.void_elem.empty(), "removed voids must not survive as stale indices");
    std::set<Index> retained(out.mesh.elem_gid.begin(), out.mesh.elem_gid.end());
    int removed = 0;
    for (std::size_t e = 0; e < original.elem_gid.size(); ++e) {
        const auto [lo, size] = bounds(original, e);
        (void)size;
        const bool void_cell = lo[0] >= .0125 - 1e-15 && lo[1] >= .0125 - 1e-15;
        require(retained.contains(original.elem_gid[e]) != void_cell,
                "L0 survival must match the original upper-right 3 x 3 columns");
        removed += void_cell;
    }
    require(removed == 18, "L0 removes exactly 18 original elements");
    for (Real vf : out.cut.sub_volfrac)
        require(vf >= 1 - 1e-12, "L0 retained cut fractions must be full");
    require(out.mesh.elem_gid.size() - out.cut.cut_elem.size() + out.cut.sub_volfrac.size() == 110,
            "L0 every retained cell has one full solid");
    valid_output(out, false);
    surfaces(out, l0_shape());
}
std::size_t row_for(const Output& out, Index gid) {
    const auto& c = out.cut;
    for (std::size_t row = 0; row < c.cut_elem.size(); ++row)
        if (out.mesh.elem_gid.at(c.cut_elem[row]) == gid)
            return row;
    throw std::runtime_error("expected cut cell gid=" + std::to_string(gid));
}
void small_count(const Output& out, std::size_t row, int expected) {
    const auto sub = out.cut.sub_off.at(row);
    require(std::count(out.cut.small_subs.begin(), out.cut.small_subs.end(), sub) == expected,
            "small membership must be decided once from completed union volume");
}
void small_subs_union() {
    // In the corner cell the L is the union of two h-by-delta strips,
    // overlapping in a delta-by-delta square: (2*h*delta-delta^2)/h^2
    // = 7.9984e-4 for h=.0025 and delta=1e-6. Each adjacent strip is 4e-4.
    const auto s = l_shape(.012501, .012501);
    auto original = background(s.outline);
    for (Real threshold : {1e-3, 5e-4}) {
        CutOptions o;
        o.small_sub_vf = threshold;
        auto out = build(s, o);
        int corners = 0, strips = 0;
        for (std::size_t e = 0; e < original.attribute.size(); ++e) {
            auto [lo, size] = bounds(original, e);
            (void)size;
            const int i = static_cast<int>(std::lround(lo[0] / .0025));
            const int j = static_cast<int>(std::lround(lo[1] / .0025));
            if (i == 5 && j == 5) {
                auto row = row_for(out, original.elem_gid[e]);
                require(out.cut.sub_off.at(row + 1) - out.cut.sub_off.at(row) == 1,
                        "corner union must produce exactly one subcell");
                near(out.cut.sub_volfrac.at(out.cut.sub_off[row]),
                     7.9984e-4,
                     1e-10,
                     "L1 corner fraction");
                std::cout << std::setprecision(17) << "L1 corner gid=" << original.elem_gid[e]
                          << " fraction=" << out.cut.sub_volfrac.at(out.cut.sub_off[row])
                          << " expected=0.00079984 threshold=" << threshold << '\n';
                small_count(out, row, threshold > 7.9984e-4 ? 1 : 0);
                ++corners;
            } else if ((i >= 6 && j == 5) || (i == 5 && j >= 6)) {
                auto row = row_for(out, original.elem_gid[e]);
                require(out.cut.sub_off.at(row + 1) - out.cut.sub_off.at(row) == 1,
                        "strip must produce exactly one subcell");
                near(
                    out.cut.sub_volfrac.at(out.cut.sub_off[row]), 4e-4, 1e-10, "L1 strip fraction");
                small_count(out, row, 1);
                ++strips;
            }
        }
        require(corners == 2 && strips == 8,
                "L1 fixture selects exactly 2 corner and 8 strip cells");
        require(out.cut.small_subs.size() == (threshold > 7.9984e-4 ? 10U : 8U),
                "L1 has no additional or duplicate small memberships");
        valid_output(out);
    }
}
// Stable node identity survives local compaction and rank-dependent storage.
void node(Bits& bits, const HexMeshArrays& m, Local v) {
    bits.add(m.node_layer.at(v));
    bits.add(m.node_column.at(v));
    bits.add(m.node_gid.at(v));
    bits.add(point(m.vertices, v));
}
void attr(Bits& bits, const HexMeshArrays& m, Local a) {
    bits.add(m.attr_table.entries.at(a - 1));
    for (int k = 0; k < 9; ++k)
        bits.add(m.attr_frame.at(9 * (a - 1) + k));
}
void boundary_attr(Bits& bits, const HexMeshArrays& m, Local a) {
    auto it = m.attr_table.boundary_names.find(a);
    require(it != m.attr_table.boundary_names.end(), "boundary attribute resolves to named entity");
    bits.add(it->second);
}
template <class T>
void slice(Bits& bits, const std::vector<T>& values, std::size_t begin, std::size_t end) {
    bits.add(end - begin);
    for (auto i = begin; i < end; ++i)
        bits.add(values.at(i));
}
Bits cell_bits(const Output& out, std::size_t e) {
    const auto& m = out.mesh;
    const auto& c = out.cut;
    Bits bits;
    const auto gid = m.elem_gid.at(e);
    bits.add(gid);
    attr(bits, m, m.attribute[e]);
    bits.add(std::count(m.pending_cut.begin(), m.pending_cut.end(), static_cast<Local>(e)));
    bits.add(std::count(c.void_elem.begin(), c.void_elem.end(), static_cast<Local>(e)));
    for (int k = 0; k < 8; ++k) {
        const auto v = m.hex.at(8 * e + k);
        node(bits, m, v);
        for (const auto& ls : c.level_sets) {
            bits.add(ls.body);
            bits.add(ls.phi.at(v));
        }
    }
    auto it = std::find(c.cut_elem.begin(), c.cut_elem.end(), static_cast<Local>(e));
    bits.add(it != c.cut_elem.end());
    if (it != c.cut_elem.end()) {
        const auto row = static_cast<std::size_t>(it - c.cut_elem.begin());
        bits.add(c.cut_elem_gid.at(row));
        bits.add(c.exterior_area_vector.at(row));
        bits.add(c.sub_off.at(row + 1) - c.sub_off.at(row));
        for (Local sub = c.sub_off[row]; sub < c.sub_off[row + 1]; ++sub) {
            attr(bits, m, c.sub_attribute.at(sub));
            bits.add(c.sub_volfrac.at(sub));
            bits.add(std::count(c.small_subs.begin(), c.small_subs.end(), sub));
            slice(bits, c.vq_w, c.vq_off.at(sub), c.vq_off.at(sub + 1));
            slice(bits, c.bq_w, c.bq_off.at(sub), c.bq_off.at(sub + 1));
            for (int d = 0; d < 3; ++d) {
                slice(bits, c.vq_xi[d], c.vq_off[sub], c.vq_off[sub + 1]);
                slice(bits, c.bq_xi[d], c.bq_off[sub], c.bq_off[sub + 1]);
                slice(bits, c.bq_n[d], c.bq_off[sub], c.bq_off[sub + 1]);
            }
            for (Local q = c.bq_off[sub]; q < c.bq_off[sub + 1]; ++q)
                boundary_attr(bits, m, c.bq_bdr_attr.at(q));
        }
        slice(bits, c.sq_w, c.sq_off.at(row), c.sq_off.at(row + 1));
        slice(bits, c.sq_side, c.sq_off[row], c.sq_off[row + 1]);
        for (const auto& coords : c.sq_xi)
            slice(bits, coords, c.sq_off[row], c.sq_off[row + 1]);
    }
    std::size_t eb_count = 0;
    for (auto parent : c.eb_parent_elem)
        eb_count += parent == static_cast<Local>(e);
    bits.add(eb_count);
    for (std::size_t v = 0; v < c.eb_parent_elem.size(); ++v)
        if (c.eb_parent_elem[v] == static_cast<Local>(e)) {
            bits.add(gid);
            bits.add(point(c.eb_points, v));
            bits.add(point(c.eb_xi, v));
            bits.add(point(c.eb_normals, v));
        }
    require(c.polyhedra.has_value(), "partition fixture exports polyhedra");
    const auto& p = *c.polyhedra;
    auto poly_it = std::find(p.parent_elem_gid.begin(), p.parent_elem_gid.end(), gid);
    bits.add(poly_it != p.parent_elem_gid.end());
    if (poly_it != p.parent_elem_gid.end()) {
        auto poly = static_cast<std::size_t>(poly_it - p.parent_elem_gid.begin());
        bits.add(p.body.at(poly));
        bits.add(p.layer_id.at(poly));
        bits.add(p.material_id.at(poly));
        bits.add(p.cell_kind.at(poly));
        bits.add(p.volume.at(poly));
        bits.add(p.volfrac_ref.at(poly));
        bits.add(p.poly_off.at(poly + 1) - p.poly_off.at(poly));
        std::set<Index> faces, points;
        for (Index f = p.poly_off[poly]; f < p.poly_off[poly + 1]; ++f) {
            auto face = p.poly_faces.at(f);
            faces.insert(face);
            for (Index v = p.face_off.at(face); v < p.face_off.at(face + 1); ++v)
                points.insert(p.face_conn.at(v));
        }
        require(!faces.empty() && !points.empty(), "nonempty polyhedron slices");
        const auto face_base = *faces.begin(), point_base = *points.begin();
        require(*faces.rbegin() - face_base + 1 == static_cast<Index>(faces.size()),
                "polyhedron faces are a contiguous cell slice");
        require(*points.rbegin() - point_base + 1 == static_cast<Index>(points.size()),
                "polyhedron points are a contiguous cell slice");
        for (Index f = p.poly_off[poly]; f < p.poly_off[poly + 1]; ++f)
            bits.add(p.poly_faces.at(f) - face_base);
        // Subtract storage offsets only. Preserve point, face, and ring order.
        for (auto face : faces) {
            bits.add(p.face_off.at(face + 1) - p.face_off.at(face));
            for (Index v = p.face_off[face]; v < p.face_off[face + 1]; ++v)
                bits.add(p.face_conn.at(v) - point_base);
        }
        bits.add(points.size());
        for (auto id : points) {
            bits.add(point(p.points, id));
            bits.add(point(p.point_xi, id));
            bits.add(m.elem_gid.at(p.point_parent_elem.at(id)));
        }
    }
    // Boundary quad order and winding are retained; local node/attribute IDs resolve to entities.
    std::set<Local> cell_nodes(m.hex.begin() + 8 * e, m.hex.begin() + 8 * (e + 1));
    for (std::size_t f = 0; f < m.bdr_attribute.size(); ++f) {
        bool belongs = true;
        for (int k = 0; k < 4; ++k)
            belongs &= cell_nodes.contains(m.bdr_quad.at(4 * f + k));
        if (belongs) {
            for (int k = 0; k < 4; ++k)
                node(bits, m, m.bdr_quad[4 * f + k]);
            boundary_attr(bits, m, m.bdr_attribute[f]);
        }
    }
    for (std::size_t f = 0; f < c.gp_face_gid.size(); ++f)
        if (c.gp_elem_gid[0].at(f) == gid || c.gp_elem_gid[1].at(f) == gid) {
            bits.add(c.gp_elem_gid[0][f]);
            bits.add(c.gp_elem_gid[1][f]);
            // gp_face is a local adapter index; resolve through the stable
            // global face identity and parent-side pair below.
            bits.add(c.gp_face_gid[f]);
            bits.add(c.gp_face_minus.at(f));
            bits.add(c.gp_face_plus.at(f));
        }
    return bits;
}
void partition_compare(const Output& serial,
                       const Output& parallel,
                       const HexMeshArrays& original) {
    std::map<Index, std::size_t> a, b;
    for (std::size_t e = 0; e < serial.mesh.elem_gid.size(); ++e)
        a.emplace(serial.mesh.elem_gid[e], e);
    for (std::size_t e = 0; e < parallel.mesh.elem_gid.size(); ++e)
        b.emplace(parallel.mesh.elem_gid[e], e);
    std::size_t checked = 0;
    for (auto gid : original.elem_gid) {
        require(a.contains(gid) == b.contains(gid), "partition-independent void removal/survival");
        if (!a.contains(gid))
            continue;
        for (Local rank = 0; rank < 2; ++rank)
            if (parallel.mesh.partitioning.at(b.at(gid)) == rank) {
                auto x = cell_bits(serial, a.at(gid)), y = cell_bits(parallel, b.at(gid));
                check(x.words == y.words, "owned cell bitwise identity gid=" + std::to_string(gid));
                ++checked;
            }
    }
    require(checked == serial.mesh.elem_gid.size(),
            "every surviving owned cell compared exactly once");
    std::map<Index, Bits> nodes;
    for (std::size_t v = 0; v < serial.mesh.node_gid.size(); ++v) {
        Bits bits;
        node(bits, serial.mesh, static_cast<Local>(v));
        for (const auto& ls : serial.cut.level_sets) {
            bits.add(ls.body);
            bits.add(ls.phi.at(v));
        }
        require(nodes.emplace(serial.mesh.node_gid.at(v), std::move(bits)).second,
                "unique stable node identity");
    }
    require(nodes.size() == parallel.mesh.node_gid.size(), "partition node survival");
    for (std::size_t v = 0; v < parallel.mesh.node_gid.size(); ++v) {
        Bits bits;
        node(bits, parallel.mesh, static_cast<Local>(v));
        for (const auto& ls : parallel.cut.level_sets) {
            bits.add(ls.body);
            bits.add(ls.phi.at(v));
        }
        require(nodes.at(parallel.mesh.node_gid.at(v)).words == bits.words,
                "stable-node coordinates and level sets bitwise");
    }
    exact(serial.mesh.attr_table, parallel.mesh.attr_table, "partition attribute tables");
    exact(serial.mesh.preparation_report, parallel.mesh.preparation_report, "partition reports");
    exact(serial.mesh.face_sets, parallel.mesh.face_sets, "partition mesh face sets");
    exact(serial.cut.face_sets, parallel.cut.face_sets, "partition cut face sets");
    exact(serial.mesh.axes, parallel.mesh.axes, "partition grid axes");
    exact(serial.mesh.copper_phi, parallel.mesh.copper_phi, "partition untouched copper field");
    exact(serial.mesh.nodes, parallel.mesh.nodes, "partition untouched nodes");
}
void determinism(const Shape& s) {
    const auto expected = build(s);
    auto compare = [&](const Shape& variant) {
        const auto actual = build(variant);
        exact(actual.mesh, expected.mesh, "deterministic full mesh");
        exact(actual.cut, expected.cut, "deterministic full cut output");
    };
    compare(s);
    auto variant = s;
    std::reverse(variant.outline.begin(), variant.outline.end());
    compare(variant);
    variant = s;
    std::rotate(variant.outline.begin(), variant.outline.begin() + 2, variant.outline.end());
    compare(variant);
    variant = s;
    variant.outline.push_back(variant.outline.front());
    compare(variant);
    // Rotate the open reversed ring, then explicitly close it again.
    variant = s;
    std::reverse(variant.outline.begin(), variant.outline.end());
    std::rotate(variant.outline.begin(), variant.outline.begin() + 2, variant.outline.end());
    variant.outline.push_back(variant.outline.front());
    compare(variant);
}
void partition_identity(const Shape& s) {
    const auto original = background(s.outline);
    const auto serial = build(s, {}, 1), parallel = build(s, {}, 2);
    partition_compare(serial, parallel, original);
    near(value(cut_stats(parallel.mesh, parallel.cut), "partition cut_stats").solid_volume,
         s.volume,
         1e-10,
         "partition analytic volume");
}
// Seed fields that clipping should leave alone, so rollback covers metadata as
// well as the geometry arrays. These are synthetic values, never real-case data.
void rollback_sentinels(HexMeshArrays& m) {
    m.copper_phi = {-0., .125};
    m.copper_layer_of_layer = {-1};
    m.nodes = {-0., .25};
    m.snapped.push_back({'x', -0., 0., -0.});
    m.grid_report.warnings.push_back("synthetic rollback sentinel");
    m.preparation_report.push_back({"rollback", {"substrate"}, {"void"}, 1, 2, .25, -0.});
    m.pending_cut = {0, 1};
}
void error_case(HexMeshArrays m,
                const Footprint& outline,
                std::string_view path,
                std::string_view reason,
                const CutOptions& options = {},
                const CutGeometry& extra = {},
                bool legacy = false) {
    const auto before = m;
    const auto result = legacy ? apply_outline_cut(m, outline, options, extra)
                               : general::apply_outline_cut_general(m, outline, options, extra);
    if (!result)
        std::cout << "returned " << result.error().path << ": " << result.error().reason << '\n';
    // Check rollback even when the implementation diagnoses the wrong error.
    exact(m, before, "all mesh fields must roll back on outline failure");
    require(!result, "invalid outline must fail");
    check(result.error().path == path,
          "error path: expected " + std::string(path) + ", got " + result.error().path);
    if (!reason.empty())
        check(result.error().reason == reason,
              "error reason: expected " + std::string(reason) + ", got " + result.error().reason);
    else
        require(!result.error().reason.empty(), "degenerate error must have a reason");
}
void legacy_L_error() {
    auto m = background(l_shape().outline);
    rollback_sentinels(m);
    error_case(std::move(m),
               footprint(l_shape().outline),
               "outline.unsupported",
               "non-convex outlines are not supported",
               {},
               {},
               true);
}
void bowtie_error() {
    auto m = background({});
    rollback_sentinels(m);
    error_case(std::move(m),
               footprint({{0, 0}, {.02, .02}, {.02, 0}, {0, .02}}),
               "outline.unsupported",
               "self-intersecting outline");
}
void nonzero_area_crossing_error() {
    auto m = background({});
    rollback_sentinels(m);
    // Unequal lobes have nonzero shoelace area and must reach check_simple
    // after successful normalization, rather than the zero-area fallback.
    error_case(std::move(m),
               footprint({{0, 0}, {.02, .02}, {.02, 0}, {0, .01}}),
               "outline.unsupported",
               "self-intersecting outline");
}
void disconnected_error() {
    auto s = r_shape();
    s.outline[1][0] = s.outline[2][0] = .0105;
    s.outline[7][0] = s.outline[8][0] = .0115;
    auto m = background(s.outline);
    rollback_sentinels(m);
    error_case(std::move(m),
               footprint(s.outline),
               "outline.unsupported",
               "outline: disconnected solid in one cell");
}
class Fault {
    std::optional<std::string> saved_;
    static int set(const char* value) {
#ifdef _WIN32
        return _putenv_s("CHIPCAE_TEST_CUT_UNION_FAIL", value ? value : "");
#else
        return value ? setenv("CHIPCAE_TEST_CUT_UNION_FAIL", value, 1)
                     : unsetenv("CHIPCAE_TEST_CUT_UNION_FAIL");
#endif
    }

public:
    explicit Fault(const char* stage) {
        if (const char* old = std::getenv("CHIPCAE_TEST_CUT_UNION_FAIL"))
            saved_ = old;
        require(set(stage) == 0, "set union fault stage");
    }
    ~Fault() { set(saved_ ? saved_->c_str() : nullptr); }
    Fault(const Fault&) = delete;
    Fault& operator=(const Fault&) = delete;
};
void late_failure(const char* stage) {
    const auto s = l_shape();
    auto m = background(s.outline);
    std::size_t void_e = m.attribute.size(), cut_e = m.attribute.size();
    for (std::size_t e = 0; e < m.attribute.size(); ++e) {
        const auto [lo, size] = bounds(m, e);
        (void)size;
        if (lo[0] > .017 && lo[1] > .017)
            void_e = e;
        if (lo[0] > .014 && lo[0] < .016 && lo[1] > .011 && lo[1] < .014)
            cut_e = e;
    }
    require(void_e < m.attribute.size() && cut_e < m.attribute.size(),
            "late failure needs void and cut cells");
    auto swap_cell = [&](std::size_t a, std::size_t b) {
        for (int j = 0; j < 8; ++j)
            std::swap(m.hex[8 * a + j], m.hex[8 * b + j]);
        std::swap(m.attribute[a], m.attribute[b]);
        std::swap(m.elem_gid[a], m.elem_gid[b]);
        std::swap(m.partitioning[a], m.partitioning[b]);
    };
    swap_cell(0, void_e);
    if (cut_e == 0)
        cut_e = void_e;
    swap_cell(1, cut_e);
    rollback_sentinels(m);
    const Fault fault(stage);
    error_case(std::move(m),
               footprint(s.outline),
               "outline",
               std::string("cut/union/test: injected ") + stage + " after output and pending void");
}
} // namespace

int main(int argc, char** argv) {
    int cases = 0, failures = 0;
    const int harness = support::run([&] {
        auto run = [&](const std::string& name, const std::function<void()>& test) {
            if (argc > 1 && name != argv[1])
                return;
            std::cout << "RUN " << name << std::endl;
            ++cases;
            try {
                test();
                std::cout << "PASS " << name << '\n';
            } catch (const std::exception& e) {
                std::cerr << "FAIL " << name << ": " << e.what() << '\n';
                ++failures;
            }
        };
        run("volume_L", [] { volume_case(l_shape()); });
        run("volume_L0", [] { volume_case(l0_shape()); });
        run("volume_U", [] { volume_case(u_shape()); });
        run("volume_V", [] { volume_case(v_shape()); });
        run("volume_R", [] { volume_case(r_shape()); });
        run("extra_planes_two_bodies", extra_planes_two_bodies);
        run("grid_aligned_L0", grid_aligned_L0);
        run("small_subs_union", small_subs_union);
        run("determinism_L", [] { determinism(l_shape()); });
        run("determinism_R", [] { determinism(r_shape()); });
        run("partition_identity_L", [] { partition_identity(l_shape()); });
        run("partition_identity_V", [] { partition_identity(v_shape()); });
        run("partition_identity_R", [] { partition_identity(r_shape()); });
        run("errors_legacy_L", legacy_L_error);
        run("errors_bowtie", bowtie_error);
        run("errors_nonzero_area_crossing", nonzero_area_crossing_error);
        run("errors_disconnected_R", disconnected_error);
        for (const char* stage : {"clipping", "export", "final_validation"})
            run(std::string("rollback_") + stage, [=] { late_failure(stage); });
        require(cases > 0, "unknown subcase filter");
        std::cout << cases << " cases, " << failures << " failed\n";
    });
    return harness == 0 && failures == 0 ? 0 : 1;
}
