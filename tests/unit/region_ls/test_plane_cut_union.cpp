#include "chipcae/mesh/cut/cut_stats.hpp"
#include "chipcae/mesh/cut/detail/plane_cut_union.hpp"
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
#include <type_traits>

// Implementation-local test seam, deliberately absent from installed headers.
// The implementation must delegate this probe to its actual internal-cap predicate.
namespace chipcae::mesh::cut::detail {
Expected<bool> union_caps_connected_for_test(const std::vector<std::array<Real, 3>>& a,
                                             const std::array<Real, 3>& an,
                                             const std::vector<std::array<Real, 3>>& b,
                                             const std::array<Real, 3>& bn,
                                             Real tau,
                                             Real length);
// Clip/clean one piece using the real helper, then serialize its ordered sources:
// {-1,-1,-1}=hex, {0,k,i}=piece, {1,j,-1}=original extra index.
Expected<std::vector<std::array<Index, 3>>> union_face_sources_for_test(const HexMeshArrays& mesh,
                                                                        Local elem,
                                                                        const UnionPiece& piece,
                                                                        std::size_t k,
                                                                        const CutGeometry& extra,
                                                                        const CutOptions& options);
} // namespace chipcae::mesh::cut::detail

namespace {
using namespace chipcae;
using namespace chipcae::mesh;
using namespace chipcae::mesh::cut;
namespace union_kernel = chipcae::mesh::cut::detail;
using sp5::require;
using union_kernel::UnionPiece;
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

// Recursive value serialization: no padding, addresses, decimal formatting, or
// floating point ==. All Real comparisons preserve signed zero and NaN payloads.
struct Bits {
    std::vector<std::uint64_t> words;
    template <class T>
        requires std::is_arithmetic_v<T>
    void add(T v) {
        if constexpr (std::is_floating_point_v<T>)
            words.push_back(std::bit_cast<std::uint64_t>(v));
        else
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

StackModel stack() {
    StackModel s;
    s.id = "cut-union-synthetic";
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
Real area(const Ring& ring) {
    Real sum = 0;
    for (std::size_t i = 0; i < ring.size(); ++i) {
        const auto a = ring[i], b = ring[(i + 1) % ring.size()];
        sum += a[0] * b[1] - a[1] * b[0];
    }
    return sum / 2;
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
bool original_edge(P2 a, P2 b, const Ring& outline) {
    for (std::size_t i = 0; i < outline.size(); ++i)
        if (on_edge(a, outline[i], outline[(i + 1) % outline.size()]) &&
            on_edge(b, outline[i], outline[(i + 1) % outline.size()]))
            return true;
    return false;
}
bool box_edge(P2 a, P2 b) {
    for (int d = 0; d < 2; ++d)
        for (Real x : {0., .02})
            if (std::abs(a[d] - x) < 1e-14 && std::abs(b[d] - x) < 1e-14)
                return true;
    return false;
}
UnionPiece piece(const Ring& ring, const Ring& outline) {
    UnionPiece p;
    for (std::size_t i = 0; i < ring.size(); ++i) {
        auto a = ring[i], b = ring[(i + 1) % ring.size()];
        Real nx = b[1] - a[1], ny = a[0] - b[0], length = std::hypot(nx, ny);
        Plane plane;
        plane.n = {nx / length, ny / length, 0};
        plane.d = plane.n[0] * a[0] + plane.n[1] * a[1];
        p.planes.push_back(plane);
        p.internal.push_back(!original_edge(a, b, outline));
    }
    return p;
}
struct Shape {
    Ring outline;
    std::vector<Ring> blocks;
    Real volume, embedded_area;
};
Shape l_shape(Real x = .0113, Real y = .0137) {
    return {{{0, 0}, {.02, 0}, {.02, y}, {x, y}, {x, .02}, {0, .02}},
            {{{0, 0}, {.02, 0}, {.02, y}, {x, y}}, {{0, 0}, {x, y}, {x, .02}, {0, .02}}},
            3.4519e-7,
            1.5e-5};
}
Shape v_shape() {
    return {{{0, 0}, {.0064, 0}, {.0103, .0087}, {.0142, 0}, {.02, 0}, {.02, .02}, {0, .02}},
            {{{0, 0}, {.0064, 0}, {.0103, .0087}, {0, .02}},
             {{.0103, .0087}, {.0142, 0}, {.02, 0}, {.02, .02}},
             {{.0103, .0087}, {.02, .02}, {0, .02}}},
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
            {{{0, 0}, {.0086, 0}, {.0086, .0043}, {.0043, .0043}},
             {{0, 0}, {.0043, .0043}, {.0043, .0157}, {0, .02}},
             {{0, .02}, {.0043, .0157}, {.0157, .0157}, {.02, .02}},
             {{.02, .02}, {.0157, .0157}, {.0157, .0043}, {.02, 0}},
             {{.0117, 0}, {.02, 0}, {.0157, .0043}, {.0117, .0043}}},
            2.5671e-7,
            5.11e-5};
}
std::vector<UnionPiece> pieces(const Shape& shape) {
    std::vector<UnionPiece> out;
    for (const auto& ring : shape.blocks)
        out.push_back(piece(ring, shape.outline));
    return out;
}
std::vector<Real> phi(const HexMeshArrays& m, const Ring& outline) {
    std::vector<Real> out;
    for (std::size_t i = 0; i < m.vertices.size(); i += 3)
        out.push_back(signed_distance({m.vertices[i], m.vertices[i + 1]}, outline));
    return out;
}
CutOptions options() {
    CutOptions o;
    o.export_polyhedra = true;
    return o;
}
struct Output {
    HexMeshArrays mesh;
    CutLayerArrays cut;
};
Output
build(const Shape& s, CutOptions o = options(), int ranks = 1, const CutGeometry& extra = {}) {
    auto m = sp5::grid(stack(), 8, ranks);
    auto c = value(union_kernel::build_cut_layer_union(m, pieces(s), extra, o, phi(m, s.outline)),
                   "build_cut_layer_union");
    return {std::move(m), std::move(c)};
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
        P3 total = c.exterior_area_vector.at(row);
        const auto sub = static_cast<std::size_t>(c.sub_off.at(row));
        for (Local q = c.bq_off.at(sub); q < c.bq_off.at(sub + 1); ++q) {
            bq += c.bq_w.at(q);
            const auto n = reference(c.bq_n, q);
            exterior_point(physical(m, e, reference(c.bq_xi, q)), s.outline, n);
            for (int d = 0; d < 3; ++d)
                total[d] += n[d] * c.bq_w[q];
        }
        const auto [lo, size] = bounds(m, e);
        (void)lo;
        const Real max_face = std::max({size[0] * size[1], size[0] * size[2], size[1] * size[2]});
        for (Real r : total)
            require(std::abs(r) <= 1e-12 * max_face, "surface/exterior area vector closure");
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
    near(sq, s.embedded_area, 1e-10, "embedded sq area");
    near(bq, s.embedded_area, 1e-10, "bq area");
    near(eb, s.embedded_area, 1e-10, "EB area");
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
            exact(ls.phi[v], expected, "ordered max of outline distance and applicable extra");
        }
    }
}
void volume_case(const Shape& s) {
    auto out = build(s);
    near(value(cut_stats(out.mesh, out.cut), "cut_stats").solid_volume,
         s.volume,
         1e-10,
         "analytic union volume");
    valid_output(out);
    surfaces(out, s);
    level_sets(out, s.outline);
}

void fixture_output_oracles();
void fixture_oracles() {
    for (const auto& s : {l_shape(), v_shape(), r_shape()}) {
        near(area(s.outline) * .001, s.volume, 1e-14, "independent shoelace outline area");
        Real sum = 0, length = 0;
        for (const auto& ring : s.blocks) {
            require(area(ring) > 0, "handwritten pieces are CCW");
            sum += area(ring);
            for (std::size_t i = 0; i < ring.size(); ++i) {
                auto a = ring[i], b = ring[(i + 1) % ring.size()], c = ring[(i + 2) % ring.size()];
                require((b[0] - a[0]) * (c[1] - b[1]) - (b[1] - a[1]) * (c[0] - b[0]) >= 0,
                        "handwritten pieces are convex");
            }
        }
        near(sum * .001, s.volume, 1e-14, "independent sum of piece areas");
        for (std::size_t i = 0; i < s.outline.size(); ++i) {
            auto a = s.outline[i], b = s.outline[(i + 1) % s.outline.size()];
            if (!box_edge(a, b))
                length += std::hypot(b[0] - a[0], b[1] - a[1]);
        }
        near(length * .001, s.embedded_area, 1e-14, "independent exterior perimeter");
    }
    require(signed_distance({.005, .005}, l_shape().outline) < 0 &&
                signed_distance({.015, .018}, l_shape().outline) > 0,
            "signed-distance parity has correct signs");
    auto m = sp5::grid(stack(), 8);
    require(m.attribute.size() == 128, "S: 8 x 8 x 2 cells");
    require(m.node_gid.size() == 243, "S: 9 x 9 x 3 nodes");
    for (std::size_t e = 0; e < m.attribute.size(); ++e) {
        auto [lo, size] = bounds(m, e);
        (void)lo;
        near(size[0], .0025, 1e-14, "S h_x");
        near(size[1], .0025, 1e-14, "S h_y");
        near(size[2], .0005, 1e-14, "S h_z");
    }
    Bits plus, minus_zero;
    plus.add(0.);
    minus_zero.add(-0.);
    require(plus.words != minus_zero.words, "bitwise oracle distinguishes signed zero");
    fixture_output_oracles();
}

void union_extra_plane() {
    auto original = sp5::grid(stack(), 8);
    const Real z0 = *std::min_element(original.axes[2].begin(), original.axes[2].end());
    CutGeometry extra{{Plane{0, {0, 0, 1}, z0 + .0005}}};
    auto s = l_shape();
    auto out = build(s, options(), 1, extra);
    near(value(cut_stats(out.mesh, out.cut), "cut_stats").solid_volume,
         1.72595e-7,
         1e-10,
         "L plus horizontal extra volume");
    valid_output(out);
    level_sets(out, s.outline, extra);
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
void union_small_subs() {
    const auto s = l_shape(.012501, .012501);
    auto original = sp5::grid(stack(), 8);
    for (Real threshold : {1e-3, 5e-4}) {
        auto o = options();
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
                near(out.cut.sub_volfrac.at(out.cut.sub_off[row]),
                     7.9984e-4,
                     1e-10,
                     "L1 corner fraction");
                small_count(out, row, threshold > 7.9984e-4 ? 1 : 0);
                ++corners;
            } else if ((i >= 6 && j == 5) || (i == 5 && j >= 6)) {
                auto row = row_for(out, original.elem_gid[e]);
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

void single_piece_bitwise() {
    const Ring ring{{.003, 0},
                    {.017, 0},
                    {.02, .003},
                    {.02, .017},
                    {.017, .02},
                    {.003, .02},
                    {0, .017},
                    {0, .003}};
    const std::vector<UnionPiece> ps{piece(ring, ring)};
    for (bool keep_void : {false, true})
        for (int degree : {2, 6}) {
            auto a = sp5::grid(stack(), 8), b = a;
            // Extra body registry order is deliberately different from numeric ID order.
            a.attr_table.bodies["aaa-unused"] = 73;
            b = a;
            CutGeometry expanded;
            for (const auto& plane : ps[0].planes)
                for (const auto& [name, body] : a.attr_table.bodies) {
                    (void)name;
                    auto p = plane;
                    p.body = body;
                    expanded.planes.push_back(p);
                }
            CutGeometry extra{{Plane{0, {0, 0, 2}, .0015}}};
            expanded.planes.insert(expanded.planes.end(), extra.planes.begin(), extra.planes.end());
            auto o = options();
            o.keep_void = keep_void;
            o.quad_degree = degree;
            auto expected = value(build_cut_layer(b, expanded, o), "legacy octagon");
            // Values are ignored on the fast path, but length is still checked.
            std::vector<Real> ignored(a.node_gid.size(), -0.);
            auto actual = value(union_kernel::build_cut_layer_union(a, ps, extra, o, ignored),
                                "single piece");
            exact(a, b, "single-piece full mesh");
            exact(actual, expected, "single-piece all cut fields");
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
            bits.add(c.gp_face.at(f));
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
    exact(serial.mesh.copper_layer_of_layer,
          parallel.mesh.copper_layer_of_layer,
          "partition copper layer map");
    exact(serial.mesh.attr_frame, parallel.mesh.attr_frame, "partition attribute frames");
    exact(serial.mesh.nodes, parallel.mesh.nodes, "partition untouched nodes");
    exact(
        serial.mesh.internal_interfaces, parallel.mesh.internal_interfaces, "partition interfaces");
    exact(serial.mesh.order, parallel.mesh.order, "partition mesh order");
    exact(serial.mesh.snapped, parallel.mesh.snapped, "partition snap reports");
    auto serial_report = serial.mesh.grid_report, parallel_report = parallel.mesh.grid_report;
    // Ownership and load imbalance depend on the rank count; all geometric
    // grid metadata and removal counts must still match bit for bit.
    serial_report.column_ranks.clear();
    parallel_report.column_ranks.clear();
    serial_report.partition_imbalance = parallel_report.partition_imbalance = 0;
    exact(serial_report, parallel_report, "partition grid reports except ownership");
    exact(serial.cut.void_attribute, parallel.cut.void_attribute, "partition void attribute");
}
void partition_identity(const Shape& s) {
    auto original = sp5::grid(stack(), 8);
    for (bool keep_void : {false, true}) {
        auto o = options();
        o.keep_void = keep_void;
        auto a = build(s, o, 1), b = build(s, o, 2);
        partition_compare(a, b, original);
        near(value(cut_stats(b.mesh, b.cut), "partition stats").solid_volume,
             s.volume,
             1e-10,
             "partition analytic volume");
    }
}

void partition_oracle_fields() {
    auto original = sp5::grid(stack(), 8);
    auto serial = build(l_shape(), options(), 1), parallel = build(l_shape(), options(), 2);
    auto rejects = [&](const std::function<void(Output&)>& mutate) {
        auto changed = parallel;
        mutate(changed);
        bool rejected = false;
        try {
            partition_compare(serial, changed, original);
        } catch (const std::exception&) {
            rejected = true;
        }
        require(rejected, "partition oracle detects a changed mesh or cut field");
    };
    rejects([](Output& out) { ++out.mesh.order; });
    rejects([](Output& out) { out.mesh.internal_interfaces = !out.mesh.internal_interfaces; });
    rejects([](Output& out) { out.mesh.copper_layer_of_layer.push_back(-1); });
    rejects([](Output& out) { out.mesh.snapped.push_back({'x', -0., 0., -0.}); });
    rejects([](Output& out) { ++out.mesh.grid_report.void_cells; });
    rejects([](Output& out) { ++out.cut.void_attribute; });
}

void error_case(HexMeshArrays mesh,
                const std::vector<UnionPiece>& ps,
                const CutGeometry& extra,
                CutOptions o,
                const std::vector<Real>& outline_phi,
                std::string_view path,
                std::string_view reason) {
    const auto before = mesh;
    const auto result = union_kernel::build_cut_layer_union(mesh, ps, extra, o, outline_phi);
    exact(mesh, before, "error must roll back every mesh field and array order");
    require(!result, "invalid/disconnected union must fail");
    check(result.error().path == path && result.error().reason == reason,
          "expected " + std::string(path) + ": " + std::string(reason) + "; got " +
              result.error().path + ": " + result.error().reason);
}
void disconnected_error() {
    const Ring left{{0, 0}, {.0105, 0}, {.0105, .02}, {0, .02}},
        right{{.0115, 0}, {.02, 0}, {.02, .02}, {.0115, .02}};
    auto m = sp5::grid(stack(), 8);
    std::vector<Real> outline_phi;
    for (std::size_t v = 0; v < m.node_gid.size(); ++v) {
        auto p = point(m.vertices, v);
        outline_phi.push_back(
            std::min(signed_distance({p[0], p[1]}, left), signed_distance({p[0], p[1]}, right)));
    }
    error_case(m,
               {piece(left, left), piece(right, right)},
               {},
               options(),
               outline_phi,
               "outline.unsupported",
               "outline: disconnected solid in one cell");
}
void invalid_piece_case(int which) {
    auto m = sp5::grid(stack(), 8);
    auto ps = pieces(l_shape());
    auto ph = phi(m, l_shape().outline);
    if (which == 0)
        ps.clear();
    if (which == 1)
        ps[1].internal.pop_back();
    if (which == 2)
        ps[0].planes[0].body = m.attr_table.entries[0].body;
    if (which == 3)
        ps[0].internal.push_back(false);
    error_case(m, ps, {}, options(), ph, "cut/union", "invalid piece");
}
void invalid_phi_case(bool fast, bool too_long) {
    auto s = l_shape();
    auto m = sp5::grid(stack(), 8);
    auto ps = pieces(s);
    if (fast) {
        ps.resize(1);
        ps[0].internal.assign(ps[0].planes.size(), false);
    }
    auto ph = phi(m, s.outline);
    if (too_long)
        ph.push_back(0);
    else
        ph.pop_back();
    error_case(m, ps, {}, options(), ph, "cut/union", "outline_phi size mismatch");
}
void invalid_option_case(int which) {
    auto m = sp5::grid(stack(), 8);
    auto o = options();
    if (which == 0)
        o.snap_phi = -1;
    if (which == 1)
        o.small_sub_vf = 1;
    if (which == 2)
        o.quad_degree = 3;
    if (which == 3)
        o.snap_phi = std::numeric_limits<Real>::quiet_NaN();
    if (which == 4)
        o.small_sub_vf = -1;
    error_case(m,
               pieces(l_shape()),
               {},
               o,
               phi(m, l_shape().outline),
               "cut/options",
               "invalid thresholds");
}
void invalid_normal_case(int which, bool extra_plane) {
    auto m = sp5::grid(stack(), 8);
    auto ps = pieces(l_shape());
    CutGeometry extra{{Plane{0, {0, 0, 1}, .00075}}};
    auto& p = extra_plane ? extra.planes[0] : ps[1].planes[0];
    if (which == 0)
        p.n = {0, 0, 0};
    if (which == 1)
        p.n[0] = std::numeric_limits<Real>::infinity();
    if (which == 2)
        p.d = std::numeric_limits<Real>::quiet_NaN();
    error_case(m,
               ps,
               extra,
               options(),
               phi(m, l_shape().outline),
               "cut/plane",
               "nonfinite or zero normal");
}

void invalid_box(bool fast) {
    auto m = sp5::grid(stack(), 8);
    const auto s = l_shape();
    auto ps = pieces(s);
    if (fast) {
        ps.resize(1);
        ps[0].internal.assign(ps[0].planes.size(), false);
    }
    m.hex.at(m.hex.size() - 2) = m.hex.at(m.hex.size() - 8);
    error_case(m, ps, {}, options(), phi(m, s.outline), "cut/mesh", "positive affine box required");
}

// Binary-exact geometry: each half occupies 1/16 of the unit cell, together
// 1/8. Integer quadrature degree and representable thresholds isolate '<'.
HexMeshArrays unit_cell() {
    auto s = stack();
    s.outline.box = {0, 0, 8, 8};
    s.layers[0].thickness = 2;
    auto m = sp5::grid(s, 8);
    const auto [lo, size] = bounds(m, 0);
    (void)lo;
    for (int d = 0; d < 3; ++d)
        near(size[d], 1, 1e-14, "binary fixture cell edge");
    return m;
}
Shape binary_shape() {
    return {
        {{0, 0}, {8, 0}, {8, .125}, {0, .125}},
        {{{0, 0}, {8, 0}, {8, .0625}, {0, .0625}}, {{0, .0625}, {8, .0625}, {8, .125}, {0, .125}}},
        2,
        0};
}
Output binary_build(Real threshold, int ranks) {
    auto s = stack();
    s.outline.box = {0, 0, 8, 8};
    s.layers[0].thickness = 2;
    auto m = sp5::grid(s, 8, ranks);
    auto shape = binary_shape();
    auto o = options();
    o.quad_degree = 2;
    o.small_sub_vf = threshold;
    auto c =
        value(union_kernel::build_cut_layer_union(m, pieces(shape), {}, o, phi(m, shape.outline)),
              "binary threshold union");
    return {std::move(m), std::move(c)};
}
void fixture_output_oracles() {
    const auto original = unit_cell();
    auto o = options();
    o.quad_degree = 2;
    // Check the binary fixture and comparison machinery on the already working
    // convex entry point before relying on either in missing-feature assertions.
    auto legacy_strip = [&](int ranks) {
        auto s = stack();
        s.outline.box = {0, 0, 8, 8};
        s.layers[0].thickness = 2;
        auto m = sp5::grid(s, 8, ranks);
        auto c = value(build_cut_layer(m, CutGeometry{{Plane{0, {0, 1, 0}, .125}}}, o),
                       "binary fixture convex slab");
        return Output{std::move(m), std::move(c)};
    };
    auto a = legacy_strip(1), b = legacy_strip(2);
    valid_output(a);
    partition_compare(a, b, original);
    for (Real fraction : a.cut.sub_volfrac)
        exact(fraction, .125, "binary slab has exactly representable fraction");
    for (const auto& p : pieces(binary_shape())) {
        auto c = value(build_cut_layer(original, CutGeometry{p.planes}, o),
                       "binary fixture convex half");
        require(c.sub_volfrac.size() == 16, "binary half covers 8 columns and 2 layers");
        for (Real fraction : c.sub_volfrac)
            exact(fraction, .0625, "each binary convex piece is exactly 1/16");
    }
}
void ordered_piece_quadrature() {
    // Binary-exact adjacent slabs avoid snapping/serialization roundoff in the
    // reference. Their union is genuinely partial in this unit parent cell.
    const auto original = unit_cell();
    const auto ps = pieces(binary_shape());
    constexpr std::size_t target = 0;
    auto o = options();
    o.quad_degree = 2;
    o.small_sub_vf = .125;
    std::vector<Real> expected_w;
    std::array<std::vector<Real>, 3> expected_xi;
    std::vector<std::size_t> piece_off{0};
    Real expected_volume = +0.;
    const auto [lo, size] = bounds(original, target);
    (void)lo;
    const Real jac = size[0] * size[1] * size[2];
    exact(jac, 1., "ordered quadrature parent has binary-exact unit jacobian");
    for (const auto& p : ps) {
        auto convex = value(build_cut_layer(original, CutGeometry{p.planes}, o),
                            "convex reference integration including internal face");
        const auto it =
            std::find(convex.cut_elem.begin(), convex.cut_elem.end(), static_cast<Local>(target));
        require(it != convex.cut_elem.end(), "each reference piece cuts the target");
        const auto row = static_cast<std::size_t>(it - convex.cut_elem.begin());
        const auto sub = convex.sub_off.at(row);
        // jac is exactly one, so this recovers the completed physical piece
        // volume without reversing a rounded division or using exported faces.
        expected_volume += convex.sub_volfrac.at(sub);
        for (Local q = convex.vq_off.at(sub); q < convex.vq_off.at(sub + 1); ++q) {
            expected_w.push_back(convex.vq_w.at(q));
            for (int d = 0; d < 3; ++d)
                expected_xi[d].push_back(convex.vq_xi[d].at(q));
        }
        piece_off.push_back(expected_w.size());
    }
    exact(expected_volume, .125, "ordered quadrature target is exactly one eighth full");
    auto out = binary_build(.125, 1);
    const auto row = row_for(out, original.elem_gid[target]);
    const auto sub = out.cut.sub_off.at(row);
    exact(out.cut.sub_volfrac.at(sub), expected_volume / jac, "ordered convex-piece volume sum");
    const auto begin = static_cast<std::size_t>(out.cut.vq_off.at(sub));
    const auto count = static_cast<std::size_t>(out.cut.vq_off.at(sub + 1)) - begin;
    check(count == expected_w.size(),
          "ordered quadrature point count: actual=" + std::to_string(count) +
              " expected=" + std::to_string(expected_w.size()));
    auto compare = [&](const std::vector<Real>& actual,
                       const std::vector<Real>& expected,
                       const std::string& field) {
        for (std::size_t k = 0; k + 1 < piece_off.size(); ++k)
            for (auto q = piece_off[k]; q < piece_off[k + 1]; ++q) {
                const auto a = std::bit_cast<std::uint64_t>(actual.at(begin + q));
                const auto b = std::bit_cast<std::uint64_t>(expected.at(q));
                if (a == b)
                    continue;
                // Values in this binary slab fixture are nonnegative, so the
                // unsigned bit-pattern distance is their ULP distance.
                std::ostringstream message;
                message << std::setprecision(std::numeric_limits<Real>::max_digits10)
                        << "ordered quadrature " << field << " index=" << q << " piece=" << k
                        << " segment=[" << piece_off[k] << ',' << piece_off[k + 1]
                        << ") actual=" << actual.at(begin + q) << " expected=" << expected.at(q)
                        << " ulp=" << (a > b ? a - b : b - a);
                check(false, message.str());
            }
    };
    compare(out.cut.vq_w, expected_w, "vq_w");
    for (int d = 0; d < 3; ++d)
        compare(out.cut.vq_xi[d], expected_xi[d], "vq_xi[" + std::to_string(d) + "]");
}

void threshold_sum(Real threshold) {
    const auto original = unit_cell();
    auto a = binary_build(threshold, 1), b = binary_build(threshold, 2);
    partition_compare(a, b, original);
    require(a.cut.cut_elem.size() == 16, "binary strip: 8 columns times 2 layers");
    for (std::size_t row = 0; row < a.cut.cut_elem.size(); ++row) {
        near(a.cut.sub_volfrac.at(a.cut.sub_off[row]), .125, 1e-14, "binary union fraction");
        exact(a.cut.sub_volfrac.at(a.cut.sub_off[row]), .125, "binary-exact equality fixture");
        small_count(a, row, 0);
    }
    valid_output(a);
}

void coincident_and_snap(bool snap_duplicate) {
    const auto s = l_shape();
    auto m = sp5::grid(stack(), 8);
    auto ps = pieces(s);
    for (auto& p : ps) {
        for (std::size_t i = 0, n = p.planes.size(); i < n; ++i) {
            auto duplicate = p.planes[i];
            if (snap_duplicate)
                duplicate.d += 2e-15;
            p.planes.push_back(duplicate);
            // Coincidence must retain the earliest source, never this internal flag.
            p.internal.push_back(true);
        }
    }
    auto c = value(union_kernel::build_cut_layer_union(m, ps, {}, options(), phi(m, s.outline)),
                   "coincident/snap duplicate planes");
    Output out{std::move(m), std::move(c)};
    near(value(cut_stats(out.mesh, out.cut), "stats").solid_volume,
         s.volume,
         1e-10,
         "duplicate planes preserve volume");
    valid_output(out);
    surfaces(out, s);
}
void aligned_internal_and_contacts() {
    // Rectangles join on the grid line y=.0125.
    // On the adjacent grid column, the other block touches only a face/edge;
    // it must not become a zero-volume connectivity node or EB cap.
    auto s = l_shape(.0125, .0125);
    s.blocks = {{{0, 0}, {.02, 0}, {.02, .0125}, {0, .0125}},
                {{0, .0125}, {.0125, .0125}, {.0125, .02}, {0, .02}}};
    s.volume = 3.4375e-7;
    auto out = build(s);
    valid_output(out, false);
    near(value(cut_stats(out.mesh, out.cut), "stats").solid_volume,
         s.volume,
         1e-10,
         "grid-aligned internal plane volume");
    // Here exterior edges themselves lie on grid lines, so no cut cells or
    // embedded patches are required. No internal cap may create one.
    require(out.cut.cut_elem.empty() && out.cut.eb_points.empty(),
            "aligned internal/outline planes create no spurious cut cell or EB");
}

void zero_volume_pieces() {
    const auto s = l_shape();
    auto ps = pieces(s);
    // A segment and a point on the reentrant boundary have bounded xy but no
    // positive volume. They must not create extra graph nodes or exported caps.
    UnionPiece segment;
    segment.planes = {{0, {1, 0, 0}, .0113},
                      {0, {-1, 0, 0}, -.0113},
                      {0, {0, 1, 0}, .02},
                      {0, {0, -1, 0}, -.0137}};
    segment.internal.assign(4, true);
    ps.push_back(segment);
    segment.planes[2].d = .0137;
    ps.push_back(segment);
    auto m = sp5::grid(stack(), 8);
    auto c = value(union_kernel::build_cut_layer_union(m, ps, {}, options(), phi(m, s.outline)),
                   "zero-volume segment/point pieces");
    Output out{std::move(m), std::move(c)};
    near(value(cut_stats(out.mesh, out.cut), "stats").solid_volume,
         s.volume,
         1e-10,
         "zero-volume pieces do not add volume");
    valid_output(out);
    surfaces(out, s);
}

void filtered_extra_provenance() {
    const auto s = l_shape();
    auto m = sp5::grid(stack(), 8);
    const auto body = m.attr_table.entries[0].body;
    m.attr_table.bodies["aaa-unused"] = body + 101;
    m.attr_table.bodies["zzz-unused"] = body + 202;
    // Original extra index 0 is filtered. Index 1 must remain an extra cap;
    // misusing its filtered index as piece-plane provenance would suppress it.
    CutGeometry extra{{Plane{body + 101, {0, 0, 1}, -10},
                       Plane{body, {0, 0, 2}, .0015},
                       Plane{body + 202, {0, 0, -1}, 10}}};
    auto c = value(
        union_kernel::build_cut_layer_union(m, pieces(s), extra, options(), phi(m, s.outline)),
        "body-filtered extra provenance");
    Output out{std::move(m), std::move(c)};
    near(value(cut_stats(out.mesh, out.cut), "stats").solid_volume,
         2.588925e-7,
         1e-10,
         "body-filtered L at three-quarter thickness");
    Real horizontal = 0;
    for (std::size_t q = 0; q < out.cut.bq_w.size(); ++q)
        if (std::abs(out.cut.bq_n[2][q] - 1) <= 1e-12)
            horizontal += out.cut.bq_w[q];
    near(horizontal, 3.4519e-4, 1e-10, "extra cap remains exterior despite internal piece flags");
    valid_output(out);
    level_sets(out, s.outline, extra);
}
void hex_provenance() {
    const Ring box{{0, 0}, {.02, 0}, {.02, .02}, {0, .02}};
    auto p = piece(box, box);
    p.internal.assign(4, true); // all four constraints coincide with existing hex faces
    auto m = sp5::grid(stack(), 8);
    const auto before = m;
    auto c = value(union_kernel::build_cut_layer_union(m, {p}, {}, options(), phi(m, box)),
                   "coincident hex source");
    require(c.cut_elem.empty() && c.eb_points.empty(),
            "hex faces cannot be relabelled internal caps");
    require(m.elem_gid == before.elem_gid && m.hex == before.hex, "hex union retains full mesh");
    near(value(cut_stats(m, c), "stats").solid_volume, 4e-7, 1e-10, "hex provenance full volume");
}

void face_source_indices() {
    const auto m = unit_cell();
    const Ring ring{{0, 0}, {.75, 0}, {.75, 1}, {0, 1}};
    auto p = piece(ring, ring);
    // These planes coincide with hex faces. Their internal flags must never
    // relabel those existing faces. Plane 1 creates a cap subsequently clipped
    // by the active extra, and must retain its original (k=7,i=1).
    p.internal = {true, false, true, true};
    const auto body = m.attr_table.entries.at(m.attribute.at(0) - 1).body;
    const CutGeometry extra{{Plane{body + 101, {1, 0, 0}, .125},
                             Plane{body, {0, 0, 1}, .625},
                             Plane{body, {0, 0, 1}, .625}}};
    const auto actual =
        value(union_kernel::union_face_sources_for_test(m, 0, p, 7, extra, options()),
              "face provenance through clipping and coincident planes");
    // Four surviving hex fragments, the clipped piece cap, then extra[1].
    // Filtering extra[0] must not renumber j; coincident extra[2] keeps source 1.
    const std::vector<std::array<Index, 3>> expected{
        {-1, -1, -1}, {-1, -1, -1}, {-1, -1, -1}, {-1, -1, -1}, {0, 7, 1}, {1, 1, -1}};
    exact(actual, expected, "ordered original face source indices");
}

void cap_probe(const std::vector<P3>& a,
               P3 an,
               const std::vector<P3>& b,
               P3 bn,
               Real tau,
               bool expected,
               std::string_view context,
               Real length = 1) {
    // Rotate the same geometry through all three projection axes.
    for (int axis = 0; axis < 3; ++axis) {
        auto rotate = [axis](P3 p) {
            std::rotate(p.begin(), p.begin() + axis, p.end());
            return p;
        };
        std::vector<P3> f, g;
        for (auto p : a)
            f.push_back(rotate(p));
        for (auto p : b)
            g.push_back(rotate(p));
        bool actual = value(
            union_kernel::union_caps_connected_for_test(f, rotate(an), g, rotate(bn), tau, length),
            context);
        check(actual == expected, std::string(context) + ": internal-cap connectivity predicate");
    }
}
void connectivity_distance(int side) {
    constexpr Real tau = 1. / 1024;
    const Real delta = side < 0 ? tau / 2 : side > 0 ? 2 * tau : tau;
    const std::vector<P3> a{{.5, 0, 0}, {.5, 1, 0}, {.5, 1, 1}, {.5, 0, 1}},
        b{{.5 + delta, 0, 1}, {.5 + delta, 1, 1}, {.5 + delta, 1, 0}, {.5 + delta, 0, 0}};
    cap_probe(a, {1, 0, 0}, b, {-1, 0, 0}, tau, side <= 0, "distance below/equal/above tau");
}
void connectivity_normal(int side) {
    // Choose the bound from the measured mismatch of two unit normals, avoiding
    // a rounded sin/cos angle that accidentally changes the equality case.
    constexpr Real angle = 1. / 1024;
    const P3 an{1, 0, 0}, bn{-std::cos(angle), std::sin(angle), 0};
    const Real mismatch = norm({an[0] + bn[0], an[1] + bn[1], 0});
    const Real tau = side < 0 ? 2 * mismatch : side > 0 ? mismatch / 2 : mismatch;
    std::vector<P3> a{{.5, -.125, 0}, {.5, .125, 0}, {.5, .125, .25}, {.5, -.125, .25}}, b;
    for (const auto& p : a)
        b.push_back({.5 + std::tan(angle) * p[1], p[1], p[2]});
    std::reverse(b.begin(), b.end());
    cap_probe(a, an, b, bn, tau, side <= 0, "normal mismatch below/equal/above tau/L");
}
void connectivity_overlap(int side) {
    // Two large caps overlap in a small isosceles triangle. At equality its
    // base=2*tau, height=tau, area=tau^2; all three edges exceed tau, so the
    // deduplication itself cannot collapse the threshold fixture.
    constexpr Real tau = 1. / 64;
    const Real h = side < 0 ? tau / 2 : side > 0 ? 2 * tau : tau;
    const std::vector<P3> a{{.5, -.125, -.125}, {.5, .125, -.125}, {.5, 0, h}};
    // An independent exact fixture uses a rectangular half-plane polygon for G:
    // clipping F at z=0 yields base 2*h*.125/(h+.125), not 2*tau. Set F's
    // bottom width so that its cross-section at zero is exactly 2*tau.
    auto f = a;
    const Real width = tau * (h + .125) / h;
    f[0][1] = -width;
    f[1][1] = width;
    const std::vector<P3> g{{.5, -.5, 0}, {.5, -.5, .5}, {.5, .5, .5}, {.5, .5, 0}};
    cap_probe(f, {1, 0, 0}, g, {-1, 0, 0}, tau, side > 0, "overlap below/equal/above tau squared");
}
void connectivity_contact(bool point_contact) {
    const std::vector<P3> a{{.5, 0, 0}, {.5, .5, 0}, {.5, .5, .5}, {.5, 0, .5}};
    const Real z = point_contact ? .5 : 0;
    const std::vector<P3> b{{.5, .5, z}, {.5, .5, z + .5}, {.5, 1, z + .5}, {.5, 1, z}};
    cap_probe(a, {1, 0, 0}, b, {-1, 0, 0}, 1. / 1024, false, "point/edge-only contact");
}

void connectivity_length_scale() {
    constexpr Real angle = 1. / 1024;
    const P3 an{1, 0, 0}, bn{-std::cos(angle), std::sin(angle), 0};
    const Real mismatch = norm({an[0] + bn[0], an[1] + bn[1], 0});
    const std::vector<P3> a{{.5, -.125, 0}, {.5, .125, 0}, {.5, .125, .25}, {.5, -.125, .25}};
    std::vector<P3> b;
    for (auto p : a)
        b.push_back({.5 + std::tan(angle) * p[1], p[1], p[2]});
    std::reverse(b.begin(), b.end());
    cap_probe(a, an, b, bn, 2 * mismatch, true, "tau/L equality for L=2", 2);
    cap_probe(a, an, b, bn, 2 * mismatch, false, "tau/L exceeded for L=4", 4);
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
};
void open_union_export_rollback() {
    const auto s = l_shape();
    auto m = sp5::grid(stack(), 8);
    const auto ph = phi(m, s.outline);
    auto ps = pieces(s);
    const auto o = options();
    auto good_mesh = m;
    auto good_cut = value(union_kernel::build_cut_layer_union(good_mesh, ps, {}, o, ph),
                          "closed union control");
    require(good_mesh.elem_gid.size() < m.elem_gid.size(), "control removes void cells");
    valid_output({std::move(good_mesh), std::move(good_cut)});

    // Incorrectly discard the first piece's exterior y=.0137 cap. At the
    // reentrant corner the second piece still supplies a retained embedded
    // face, but the exported boundary has unpaired edges and must be rejected.
    require(!ps[0].internal[2], "fixture cap starts as exterior");
    ps[0].internal[2] = true;
    m.copper_phi = {-0., .125};
    m.nodes = {-0., .25};
    m.grid_report.warnings.push_back("synthetic closure rollback sentinel");
    const auto before = m;
    auto result = union_kernel::build_cut_layer_union(m, ps, {}, o, ph);
    require(!result, "open union export must fail before committing the mesh");
    require(result.error().path == "cut/polyhedron", "open export reports polyhedron error");
    require(result.error().reason.find("closed=false") != std::string::npos,
            "unpaired exported edges fail closure validation");
    exact(m, before, "open export preserves all caller mesh fields");
}
void late_failure(const char* stage, bool fast) {
    auto m = sp5::grid(stack(), 8);
    Shape s = l_shape();
    if (fast) {
        s.outline = {{.004, .004}, {.016, .004}, {.016, .016}, {.004, .016}};
        s.blocks = {s.outline};
    }
    // A void cell precedes a cut cell in the actual traversal. The hook must
    // fire only after private output exists AND a void removal is pending.
    std::size_t void_e = m.attribute.size(), cut_e = m.attribute.size();
    for (std::size_t e = 0; e < m.attribute.size(); ++e) {
        auto [lo, size] = bounds(m, e);
        if (lo[0] > .017 && lo[1] > .017)
            void_e = e;
        if (lo[0] > .014 && lo[0] < .016 && lo[1] > .011 && lo[1] < .014)
            cut_e = e;
    }
    require(void_e < m.attribute.size() && cut_e < m.attribute.size(), "late-failure fixtures");
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
    // Nonempty untouched fields exercise recursive rollback beyond geometry.
    m.copper_phi = {-0., .125};
    m.copper_layer_of_layer = {-1};
    m.nodes = {-0., .25};
    m.snapped.push_back({'x', -0., 0., -0.});
    m.grid_report.warnings.push_back("synthetic rollback sentinel");
    m.pending_cut = {0, 1};
    const auto ph = phi(m, s.outline);
    Fault fault(stage);
    error_case(m,
               pieces(s),
               {},
               options(),
               ph,
               "cut/union/test",
               std::string("injected ") + stage + " after output and pending void");
}

} // namespace
int main(int argc, char** argv) {
    // Run all cases sequentially inside one support harness/Kokkos lifetime. Keep
    // going after a failure so the red pass reports all absent behaviors.
    int failures = 0, cases = 0;
    const int harness = sp5::run([&] {
        auto run = [&](const std::string& name, const std::function<void()>& test) {
            if (argc > 1 && name != argv[1])
                return;
            std::cout << "RUN " << name << std::endl;
            try {
                test();
            } catch (const std::exception& e) {
                std::cerr << "FAIL: " << e.what() << '\n';
                ++failures;
            }
            ++cases;
        };
        run("fixture_oracles", fixture_oracles);
        run("union_volume_L", [] { volume_case(l_shape()); });
        run("union_volume_V", [] { volume_case(v_shape()); });
        run("union_volume_R", [] { volume_case(r_shape()); });
        run("union_extra_plane", union_extra_plane);
        run("union_small_subs", union_small_subs);
        run("single_piece_bitwise", single_piece_bitwise);
        run("ordered_piece_quadrature", ordered_piece_quadrature);
        run("partition_identity_L", [] { partition_identity(l_shape()); });
        run("partition_identity_R", [] { partition_identity(r_shape()); });
        run("partition_oracle_fields", partition_oracle_fields);
        run("disconnected_error", disconnected_error);
        for (int i = 0; i < 4; ++i)
            run("invalid_piece_" + std::to_string(i), [i] { invalid_piece_case(i); });
        for (bool fast : {false, true})
            for (bool long_phi : {false, true})
                run("invalid_phi_" + std::to_string(fast) + "_" + std::to_string(long_phi),
                    [=] { invalid_phi_case(fast, long_phi); });
        for (int i = 0; i < 5; ++i)
            run("invalid_options_" + std::to_string(i), [i] { invalid_option_case(i); });
        for (int i = 0; i < 3; ++i)
            for (bool extra : {false, true})
                run("invalid_normal_" + std::to_string(i) + "_" + std::to_string(extra),
                    [=] { invalid_normal_case(i, extra); });
        run("invalid_box_union", [] { invalid_box(false); });
        run("invalid_box_fast", [] { invalid_box(true); });
        run("threshold_pieces_small_union_above", [] { threshold_sum(.09375); });
        run("threshold_union_exactly_equal", [] { threshold_sum(.125); });
        run("coincident_planes", [] { coincident_and_snap(false); });
        run("snap_duplicate_degenerate_caps", [] { coincident_and_snap(true); });
        run("grid_aligned_internal_point_edge_contacts", aligned_internal_and_contacts);
        run("zero_volume_pieces", zero_volume_pieces);
        run("filtered_extra_original_index", filtered_extra_provenance);
        run("coincident_hex_source", hex_provenance);
        run("face_source_indices", face_source_indices);
        for (int side : {-1, 0, 1}) {
            run("connectivity_distance_" + std::to_string(side),
                [=] { connectivity_distance(side); });
            run("connectivity_normal_" + std::to_string(side), [=] { connectivity_normal(side); });
            run("connectivity_overlap_" + std::to_string(side),
                [=] { connectivity_overlap(side); });
        }
        run("connectivity_point_contact", [] { connectivity_contact(true); });
        run("connectivity_edge_contact", [] { connectivity_contact(false); });
        run("connectivity_length_scale", connectivity_length_scale);
        run("open_union_export_rollback", open_union_export_rollback);
        for (const char* stage : {"clipping", "export", "final_validation"})
            for (bool fast : {false, true})
                run(std::string("rollback_") + stage + (fast ? "_fast" : "_union"),
                    [=] { late_failure(stage, fast); });
        require(cases > 0, "test filter must select a case");
        std::cout << cases << " cases, " << failures << " failed\n";
    });
    return harness == 0 && failures == 0 ? 0 : 1;
}
