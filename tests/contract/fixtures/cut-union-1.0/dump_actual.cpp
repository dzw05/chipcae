#include "chipcae/mesh/cut/cut_stats.hpp"
#include "chipcae/mesh/cut/detail/plane_cut_union.hpp"
#include "support.hpp"

#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iomanip>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <string_view>
#include <type_traits>
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
std::uint64_t fnv(const Bits& b) {
    std::uint64_t h = 1469598103934665603ULL;
    for (auto w : b.words) {
        h ^= w;
        h *= 1099511628211ULL;
    }
    return h;
}
void emit_planes(const std::vector<UnionPiece>& ps) {
    std::printf("\"pieces\":[");
    for (std::size_t k = 0; k < ps.size(); ++k) {
        std::printf("%s[", k ? "," : "");
        for (std::size_t i = 0; i < ps[k].planes.size(); ++i) {
            const auto& p = ps[k].planes[i];
            std::printf("%s{\"n\":[%.17g,%.17g,%.17g],\"d\":%.17g,\"internal\":%s}",
                        i ? "," : "",
                        p.n[0],
                        p.n[1],
                        p.n[2],
                        p.d,
                        ps[k].internal[i] ? "true" : "false");
        }
        std::printf("]");
    }
    std::printf("],");
}
void emit_ok(const char* name, const Output& out, const std::vector<UnionPiece>& ps, bool first) {
    const auto& c = out.cut;
    auto st = cut_stats(out.mesh, c);
    Real eb = 0;
    for (std::size_t q = 0; q < c.sq_w.size(); ++q)
        if (c.sq_side[q] == -1)
            eb += c.sq_w[q];
    std::size_t faces = c.polyhedra ? c.polyhedra->face_off.size() - 1 : 0;
    std::size_t polys = c.polyhedra ? c.polyhedra->poly_off.size() - 1 : 0;
    std::printf("%s\"%s\":{\"ok\":true,", first ? "" : ",", name);
    emit_planes(ps);
    std::printf("\"solid_volume\":%.17g,\"embedded_area\":%.17g,\"n_cells\":%zu,\"n_cut\":%zu,"
                "\"n_poly\":%zu,\"n_faces\":%zu,\"n_small\":%zu,\"cells\":[",
                st ? st.value().solid_volume : -1.0,
                eb,
                out.mesh.attribute.size(),
                c.cut_elem.size(),
                polys,
                faces,
                c.small_subs.size());
    for (std::size_t row = 0; row < c.cut_elem.size(); ++row) {
        auto [lo, size] = bounds(out.mesh, c.cut_elem[row]);
        (void)size;
        std::printf("%s[%ld,%ld,%ld,%.17g]",
                    row ? "," : "",
                    std::lround(lo[0] / .0025),
                    std::lround(lo[1] / .0025),
                    std::lround(lo[2] / .0005),
                    c.sub_volfrac.at(c.sub_off[row]));
    }
    std::printf("]}");
}
void emit_err(const char* name,
              HexMeshArrays m,
              const std::vector<UnionPiece>& ps,
              const CutGeometry& ex,
              CutOptions o,
              const std::vector<Real>& ph) {
    Bits b0;
    b0.add(m);
    auto r = union_kernel::build_cut_layer_union(m, ps, ex, o, ph);
    Bits b1;
    b1.add(m);
    std::printf(",\"%s\":{\"ok\":%s,\"path\":\"%s\",\"reason\":\"%s\",\"mesh_unchanged\":%s}",
                name,
                r ? "true" : "false",
                r ? "" : r.error().path.c_str(),
                r ? "" : r.error().reason.c_str(),
                b0.words == b1.words ? "true" : "false");
}
} // namespace
int main() {
    return sp5::run([] {
        std::printf("{");
        auto L = l_shape(), V = v_shape(), R = r_shape();
        emit_ok("L", build(L), pieces(L), true);
        emit_ok("V", build(V), pieces(V), false);
        emit_ok("R", build(R), pieces(R), false);
        auto L1 = l_shape(.012501, .012501);
        for (Real th : {1e-3, 5e-4}) {
            auto o = options();
            o.small_sub_vf = th;
            emit_ok(th > 7e-4 ? "L1_th1e-3" : "L1_th5e-4", build(L1, o), pieces(L1), false);
        }
        { // convex single piece vs legacy build_cut_layer (bitwise)
            const Ring ring{{.003, 0},
                            {.017, 0},
                            {.02, .003},
                            {.02, .017},
                            {.017, .02},
                            {.003, .02},
                            {0, .017},
                            {0, .003}};
            const std::vector<UnionPiece> ps{piece(ring, ring)};
            auto a = sp5::grid(stack(), 8), b = a;
            CutGeometry expanded;
            for (const auto& plane : ps[0].planes)
                for (const auto& [name, body] : a.attr_table.bodies) {
                    (void)name;
                    auto p = plane;
                    p.body = body;
                    expanded.planes.push_back(p);
                }
            auto o = options();
            auto e = build_cut_layer(b, expanded, o);
            std::vector<Real> ignored(a.node_gid.size(), -0.);
            auto u = union_kernel::build_cut_layer_union(a, ps, {}, o, ignored);
            Bits x, y, mx, my;
            x.add(u.value());
            y.add(e.value());
            mx.add(a);
            my.add(b);
            Output out{a, u.value()};
            emit_ok("O8_convex", out, ps, false);
            std::printf(",\"O8_bitwise\":{\"cut_equal\":%s,\"mesh_equal\":%s,\"cut_fnv\":\"%"
                        "016llx\",\"legacy_fnv\":\"%016llx\",\"n_words\":%zu}",
                        x.words == y.words ? "true" : "false",
                        mx.words == my.words ? "true" : "false",
                        (unsigned long long)fnv(x),
                        (unsigned long long)fnv(y),
                        x.words.size());
        }
        auto m = sp5::grid(stack(), 8);
        auto ph = phi(m, L.outline);
        emit_err("E_empty", m, {}, {}, options(), ph);
        {
            auto ps = pieces(L);
            ps[1].internal.pop_back();
            emit_err("E_internal_size", m, ps, {}, options(), ph);
        }
        {
            auto ps = pieces(L);
            ps[0].planes[0].body = m.attr_table.entries[0].body;
            emit_err("E_body", m, ps, {}, options(), ph);
        }
        {
            auto p2 = ph;
            p2.pop_back();
            emit_err("E_phi_short", m, pieces(L), {}, options(), p2);
        }
        {
            auto p2 = ph;
            p2.push_back(0);
            auto ps = pieces(L);
            ps.resize(1);
            ps[0].internal.assign(ps[0].planes.size(), false);
            emit_err("E_phi_long_fast", m, ps, {}, options(), p2);
        }
        {
            auto o = options();
            o.quad_degree = 3;
            emit_err("E_options", m, pieces(L), {}, o, ph);
        }
        {
            auto ps = pieces(L);
            ps[1].planes[0].n = {0, 0, 0};
            emit_err("E_plane", m, ps, {}, options(), ph);
        }
        {
            const Ring left{{0, 0}, {.0105, 0}, {.0105, .02}, {0, .02}},
                right{{.0115, 0}, {.02, 0}, {.02, .02}, {.0115, .02}};
            std::vector<Real> op;
            for (std::size_t v = 0; v < m.node_gid.size(); ++v) {
                auto p = point(m.vertices, v);
                op.push_back(std::min(signed_distance({p[0], p[1]}, left),
                                      signed_distance({p[0], p[1]}, right)));
            }
            emit_err(
                "E_disconnected", m, {piece(left, left), piece(right, right)}, {}, options(), op);
        }
        std::printf("}\n");
    });
}
