#include "chipcae/mesh/cut/cut_stats.hpp"
#include "chipcae/mesh/cut/detail/convex_pieces.hpp"
#include "chipcae/mesh/cut/detail/outline_nonconvex.hpp"
#include "chipcae/mesh/cut/detail/plane_cut_union.hpp"
#include "chipcae/mesh/cut/outline_cut.hpp"
#include "region_cut_support.hpp"

#include <bit>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <string_view>
#include <sys/resource.h>
#include <tuple>
#include <type_traits>
namespace {
using namespace chipcae;
using namespace chipcae::mesh;
using namespace chipcae::mesh::cut;
namespace union_kernel = chipcae::mesh::cut::detail;
using sp5::require;
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
    check(std::isfinite(actual) && std::abs(actual - expected) / std::abs(expected) <= rel,
          message.str());
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
    std::vector<bool> floating;
    template <class T>
        requires std::is_arithmetic_v<T>
    void add(T v) {
        floating.push_back(std::is_floating_point_v<T>);
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
            add(static_cast<T>(v));
    }
    template <class K, class V>
    void add(const std::map<K, V>& a) {
        add(a.size());
        for (const auto& [k, v] : a) {
            add(k);
            add(v);
        }
    }
    void add(const Plane& a) {
        add(a.body);
        add(a.n);
        add(a.d);
    }
    void add(const PieceUnion& a) {
        add(a.body);
        add(a.pieces);
        add(a.internal);
        add(a.ring);
    }
    void add(const CutGeometry& a) {
        add(a.planes);
        add(a.unions);
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
union_kernel::UnionPiece piece(const Ring& ring, const Ring& outline) {
    union_kernel::UnionPiece p;
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
StackModel fixture() {
    StackModel s;
    s.id = "fcbga_demo";
    s.unit = "m";
    s.symmetry = "none";
    s.package_type = "FCBGA";
    s.outline.box = {0, 0, .02, .02};
    s.outline.polygon = {{.002, 0},
                         {.018, 0},
                         {.02, .002},
                         {.02, .018},
                         {.018, .02},
                         {.002, .02},
                         {0, .018},
                         {0, .002}};
    for (const auto& [name, material, thickness, kind] :
         {std::tuple{"substrate", "organic_eq", .001, "Homogenized"},
          std::tuple{"bump_eq", "sac305_ball_eq", .0001, "Homogenized"},
          std::tuple{"die_layer", "underfill", .0005, "Explicit"}}) {
        Layer layer;
        layer.name = name;
        layer.material = material;
        layer.thickness = thickness;
        layer.kind = kind;
        layer.n_elem_min = 2;
        s.layers.push_back(layer);
    }
    s.layers.back().regions.push_back({"die", "silicon", "", {{.005, .005, .015, .015}, {}}});
    s.activation_groups["assembly"] = {"layer:substrate", "body:die"};
    return s;
}

HexMeshArrays background(const StackModel& stack) {
    auto rectangular = stack;
    rectangular.outline.polygon.clear();
    GridOptions options;
    options.nx = options.ny = 4;
    options.levels = 1;
    options.max_ratio = 10;
    options.ranks = 1;
    return value(build_tensor_grid(rectangular, options),
                 "demo-gate void digest: background construction failed");
}

CutGeometry outline_planes(const StackModel& stack, const HexMeshArrays& mesh) {
    // Reproduce apps/chipcae-solve/src/mesh_setup.cpp:32-63, without apps dependencies.
    auto ring = stack.outline.polygon;
    CutGeometry geometry;
    if (ring.empty())
        return geometry;
    if (ring.size() > 1 && ring.front() == ring.back())
        ring.pop_back();
    double area = 0;
    for (std::size_t i = 0; i < ring.size(); ++i) {
        auto a = ring[i], b = ring[(i + 1) % ring.size()];
        area += a[0] * b[1] - a[1] * b[0];
    }
    sp5::require(ring.size() >= 3 && area != 0,
                 "demo-gate void digest: outline must be a nondegenerate polygon");
    for (std::size_t i = 0; i < ring.size(); ++i) {
        auto a = ring[i], b = ring[(i + 1) % ring.size()];
        double nx = (b[1] - a[1]) * (area > 0 ? 1 : -1), ny = (a[0] - b[0]) * (area > 0 ? 1 : -1);
        double length = std::hypot(nx, ny);
        sp5::require(length != 0, "demo-gate void digest: duplicate outline vertex");
        nx /= length;
        ny /= length;
        double d = nx * a[0] + ny * a[1];
        for (auto v : ring)
            sp5::require(nx * v[0] + ny * v[1] - d <= 1e-12 * length,
                         "demo-gate void digest: non-convex outline");
        for (const auto& [name, body] : mesh.attr_table.bodies)
            geometry.planes.push_back({body, {nx, ny, 0}, d});
    }
    return geometry;
}

CutOptions options() {
    CutOptions o;
    o.export_polyhedra = true;
    o.quad_degree = 2;
    return o;
}
StackModel region_stack(const Ring& ring) {
    auto s = stack();
    Layer l;
    l.name = "lid_layer";
    l.kind = "Explicit";
    l.thickness = .0005;
    l.n_elem_min = 2;
    l.grading = 1;
    auto r = sp5::box("lid-region", 0, 0, .02, .02);
    r.body = "lid";
    r.footprint.polygon = ring;
    l.regions = {r};
    s.layers.push_back(l);
    return s;
}
void baseline(const std::filesystem::path& directory) {
    // Captured from fc719ece before any public data stub or implementation edit.
    // Every word is a recursively serialized field, never object padding.
    const std::map<std::string, std::pair<std::size_t, std::uint64_t>> expected{
        {"square_planes", {21, 9580933620311875585ULL}},
        {"square_mutable", {34496, 8818086712802570500ULL}},
        {"square_const", {34941, 6533148640578137768ULL}},
        {"chamfer_planes", {41, 16399218784753457996ULL}},
        {"chamfer_mutable", {25522, 10148799264000540274ULL}},
        {"chamfer_const", {24933, 620722291587389010ULL}},
        {"demo_planes", {161, 18386149356278925577ULL}},
        {"demo_mutable", {95021, 7991104722107197424ULL}},
        {"i10_fast", {22025, 172511145936171116ULL}}};
    auto save = [&](const std::string& name, const Bits& b) {
        std::uint64_t h = 14695981039346656037ULL;
        for (auto w : b.words)
            for (int k = 0; k < 8; ++k) {
                h ^= (w >> (8 * k)) & 255;
                h *= 1099511628211ULL;
            }
        check(expected.at(name) == std::pair{b.words.size(), h},
              "convex baseline fingerprint " + name);
        // The RED verification supplies the full baseline evidence directory too.
        // This additionally compares every field bit, without relying on a hash.
        if (!directory.empty()) {
            std::ifstream in(directory / (name + ".words"));
            require(bool(in), "open full baseline");
            std::vector<std::uint64_t> words;
            std::uint64_t w;
            while (in >> std::hex >> w)
                words.push_back(w);
            check(words == b.words, "convex baseline complete field bits " + name);
        }
        std::cout << "BASELINE " << name << " words=" << b.words.size() << " fnv=" << h << '\n';
    };
    const Ring square{{.004, .004}, {.016, .004}, {.016, .016}, {.004, .016}};
    const Ring chamfer{{.003, 0},
                       {.017, 0},
                       {.02, .003},
                       {.02, .017},
                       {.017, .02},
                       {.003, .02},
                       {0, .017},
                       {0, .003}};
    for (bool octagon : {false, true}) {
        auto s = region_stack(octagon ? chamfer : square);
        auto rectangular = s;
        rectangular.layers.back().regions[0].footprint.polygon.clear();
        auto m = sp5::grid(rectangular, 8);
        auto g = value(region_cut_planes(s, m), "convex baseline planes");
        require(g.unions.empty(), "convex producer never emits unions");
        Bits p;
        p.add(g.planes);
        save(octagon ? "chamfer_planes" : "square_planes", p);
        for (bool immutable : {false, true}) {
            auto candidate = m;
            auto c = immutable
                         ? value(build_cut_layer(
                                     static_cast<const HexMeshArrays&>(candidate), g, options()),
                                 "baseline const")
                         : value(build_cut_layer(candidate, g, options()), "baseline mutable");
            Bits b;
            b.add(candidate);
            b.add(c);
            save(std::string(octagon ? "chamfer" : "square") + (immutable ? "_const" : "_mutable"),
                 b);
        }
    }
    auto s = fixture();
    auto m = background(s);
    auto g = outline_planes(s, m);
    Bits p;
    p.add(g.planes);
    save("demo_planes", p);
    auto c = value(build_cut_layer(m, g, options()), "demo baseline");
    const auto digest = chipcae::preview::detail::digest(m);
    require(digest.cells == 1728 && digest.nodes == 2261 && c.cut_elem.size() == 66,
            "demo gate counts");
    require(digest.sha256 == "efc79a629779602538239635706a9f4293e897f8e46b3c2aeb43c22c6b6b9f4e",
            "demo gate digest");
    std::cout << "DEMO cells=" << digest.cells << " nodes=" << digest.nodes
              << " cut=" << c.cut_elem.size() << " sha256=" << digest.sha256 << '\n';
    Bits b;
    b.add(m);
    b.add(c);
    save("demo_mutable", b);
    auto fast = sp5::grid(stack(), 8);
    auto cfast =
        value(union_kernel::build_cut_layer_union(fast,
                                                  {piece(chamfer, chamfer)},
                                                  {},
                                                  options(),
                                                  std::vector<Real>(fast.node_gid.size(), -0.)),
              "I10 fast baseline");
    Bits f;
    f.add(fast);
    f.add(cfast);
    save("i10_fast", f);
}
Ring l_ring() {
    return {{0, 0}, {.02, 0}, {.02, .0137}, {.0113, .0137}, {.0113, .02}, {0, .02}};
}
Ring u_ring() {
    return {{0, 0},
            {.02, 0},
            {.02, .02},
            {.0129, .02},
            {.0129, .0123},
            {.0071, .0123},
            {.0071, .02},
            {0, .02}};
}
Ring square_ring() {
    return {{.004, .004}, {.016, .004}, {.016, .016}, {.004, .016}};
}

// This consumer fixture uses the already-tested decomposition API. Producer tests
// separately require region_cut_planes to emit the same internal flags and ring.
PieceUnion manual_union(BodyId body, const Ring& ring) {
    PieceUnion u;
    u.body = body;
    u.ring = value(union_kernel::normalize_ring(ring), "manual ring");
    const auto d = value(union_kernel::convex_pieces(u.ring), "manual decomposition");
    u.internal = d.internal_edge;
    for (const auto& block : d.pieces) {
        auto p = piece(block, u.ring);
        for (auto& plane : p.planes)
            plane.body = body;
        u.pieces.push_back(std::move(p.planes));
    }
    return u;
}
PieceUnion split_square(BodyId body) {
    PieceUnion u;
    u.body = body;
    u.ring = square_ring();
    for (const Ring& block :
         std::vector<Ring>{{{.004, .004}, {.0101, .004}, {.0101, .016}, {.004, .016}},
                           {{.0101, .004}, {.016, .004}, {.016, .016}, {.0101, .016}}}) {
        auto p = piece(block, u.ring);
        for (auto& plane : p.planes)
            plane.body = body;
        u.pieces.push_back(std::move(p.planes));
        u.internal.push_back(std::move(p.internal));
    }
    return u;
}
struct Output {
    HexMeshArrays mesh;
    CutLayerArrays cut;
};
Output direct(const HexMeshArrays& background, const CutGeometry& g, bool immutable = false) {
    auto m = background;
    auto c = immutable ? value(build_cut_layer(static_cast<const HexMeshArrays&>(m), g, options()),
                               "const union")
                       : value(build_cut_layer(m, g, options()), "mutable union");
    if (immutable)
        exact(m, background, "const union leaves every input field untouched");
    return {std::move(m), std::move(c)};
}
Real rss_mib() {
    rusage usage{};
    require(getrusage(RUSAGE_SELF, &usage) == 0, "getrusage");
#ifdef __APPLE__
    return static_cast<Real>(usage.ru_maxrss) / (1024 * 1024);
#else
    return static_cast<Real>(usage.ru_maxrss) / 1024;
#endif
}
Output
applied(const StackModel& s, const CutGeometry& g, const std::string& name, bool general = false) {
    auto rectangular = s;
    rectangular.outline.polygon.clear();
    auto m = sp5::grid(rectangular, 8);
    const auto start = std::chrono::steady_clock::now();
    auto result = general ? union_kernel::apply_outline_cut_general(m, s.outline, options(), g)
                          : apply_outline_cut(m, s.outline, options(), g);
    const Real seconds =
        std::chrono::duration<Real>(std::chrono::steady_clock::now() - start).count();
    std::cout << "MEASURE " << name << " seconds=" << std::setprecision(17) << seconds
              << " peak_rss_MiB=" << rss_mib() << '\n';
    auto c = value(std::move(result), name);
    check(c.has_value(), name + ": union-only call must return a cut layer");
    return {std::move(m), std::move(*c)};
}
void production(const Ring& ring) {
    const auto s = region_stack(ring), reverse = [&] {
        auto r = s;
        std::reverse(r.layers.back().regions[0].footprint.polygon.begin(),
                     r.layers.back().regions[0].footprint.polygon.end());
        return r;
    }();
    const auto m = sp5::grid(s, 8);
    const auto g = value(region_cut_planes(s, m), "simple nonconvex region must produce geometry");
    const auto lid = m.attr_table.bodies.at("lid");
    require(g.planes.empty(), "S prime nonconvex region produces no legacy lid planes");
    require(g.unions.size() == 1, "one union for one lid region");
    const auto& u = g.unions[0];
    require(u.body == lid, "public PieceUnion targets lid numeric BodyId");
    require(u.pieces.size() >= 2 && u.pieces.size() == u.internal.size(),
            "piece/internal outer dimensions");
    exact(u.ring, ring, "normalized CCW unsnapped source ring");
    const auto d = value(union_kernel::convex_pieces(ring), "reference decomposition");
    exact(u.internal, d.internal_edge, "internal_edge flags retained unchanged and ordered");
    for (std::size_t k = 0; k < u.pieces.size(); ++k) {
        require(!u.pieces[k].empty(), "each region convex block is nonempty");
        require(u.pieces[k].size() == u.internal[k].size(), "piece/internal inner dimensions");
        for (const auto& p : u.pieces[k])
            require(p.body == lid, "public piece planes carry target body");
        // Check the legacy edge arithmetic on exterior and internal planes alike.
        const auto& block = d.pieces.at(k);
        for (std::size_t i = 0; i < block.size(); ++i) {
            const auto a = block[i], b = block[(i + 1) % block.size()];
            const Real dx = b[0] - a[0], dy = b[1] - a[1];
            auto snap = [&](Real coordinate, int axis) {
                Real nearest = coordinate, distance = GridOptions{}.snap_tol;
                bool found = false;
                for (Real line : m.axes[axis]) {
                    const Real delta = std::abs(line - coordinate);
                    if (delta <= distance && (!found || delta < distance || line < nearest)) {
                        nearest = line;
                        distance = delta;
                        found = true;
                    }
                }
                return nearest;
            };
            Plane expected;
            expected.body = lid;
            if (dx == 0) {
                expected.n = {dy > 0 ? Real(1) : Real(-1), 0, 0};
                expected.d = expected.n[0] * snap(a[0], 0);
            } else if (dy == 0) {
                expected.n = {0, dx > 0 ? Real(-1) : Real(1), 0};
                expected.d = expected.n[1] * snap(a[1], 1);
            } else {
                const Real length = std::hypot(dx, dy);
                expected.n = {dy / length, -dx / length, 0};
                expected.d = expected.n[0] * a[0] + expected.n[1] * a[1];
            }
            exact(u.pieces[k].at(i), expected, "ordered piece planes and axis snapping");
        }
    }
    exact(g, value(region_cut_planes(reverse, m), "reversed geometry"), "reversed ring geometry");
    exact(g, value(region_cut_planes(s, m), "repeated geometry"), "same-process geometry");
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
        require(norm(residual) <= 1e-12, "exported area-vector Euclidean closure");
        require(signed_volume > 0, "positive signed nonconvex boundary volume");
        near(signed_volume, jac * c.sub_volfrac[sub], 1e-10, "boundary and ordered piece volume");
        near(p.volume.at(poly), signed_volume, 1e-10, "export volume");
        exact(p.volfrac_ref.at(poly), c.sub_volfrac[sub], "export authoritative fraction");
    }
    sp5::compact(m);
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
void closure(const Output& out) {
    valid_output(out);
    for (std::size_t row = 0; row < out.cut.cut_elem.size(); ++row) {
        P3 total = out.cut.exterior_area_vector.at(row);
        for (Local sub = out.cut.sub_off[row]; sub < out.cut.sub_off[row + 1]; ++sub)
            for (Local q = out.cut.bq_off[sub]; q < out.cut.bq_off[sub + 1]; ++q)
                for (int d = 0; d < 3; ++d)
                    total[d] += out.cut.bq_n[d][q] * out.cut.bq_w[q];
        require(norm(total) <= 1e-12,
                "each cut cell exterior_area_vector plus embedded vectors closes");
    }
}
void surface_point(P3 xyz, const Ring& ring, P3 normal) {
    bool found = false;
    for (std::size_t i = 0; i < ring.size(); ++i) {
        auto a = ring[i], b = ring[(i + 1) % ring.size()];
        if (box_edge(a, b) || !on_edge({xyz[0], xyz[1]}, a, b))
            continue;
        P3 n{b[1] - a[1], a[0] - b[0], 0};
        const auto length = norm(n);
        for (auto& x : n)
            x /= length;
        found |= norm(minus(n, normal)) <= 1e-12;
    }
    require(found,
            "EB/bq point and outward normal belong to original non-box ring, never a decomposition "
            "boundary");
}
void surfaces(const Output& out, const Ring& ring, BodyId lid, Real expected) {
    const auto& c = out.cut;
    const auto& m = out.mesh;
    Real sq = 0, bq = 0, eb = 0;
    for (std::size_t row = 0; row < c.cut_elem.size(); ++row) {
        const auto e = c.cut_elem[row];
        if (m.attr_table.entries.at(m.attribute.at(e) - 1).body != lid)
            continue;
        for (Local q = c.sq_off[row]; q < c.sq_off[row + 1]; ++q)
            if (c.sq_side.at(q) == -1) {
                sq += c.sq_w.at(q);
                const auto p = physical(m, e, reference(c.sq_xi, q));
                bool on_original = false;
                for (std::size_t k = 0; k < ring.size(); ++k)
                    on_original |= !box_edge(ring[k], ring[(k + 1) % ring.size()]) &&
                                   on_edge({p[0], p[1]}, ring[k], ring[(k + 1) % ring.size()]);
                require(on_original, "side -1 quadrature excludes decomposition caps");
            }
        for (Local sub = c.sub_off[row]; sub < c.sub_off[row + 1]; ++sub)
            for (Local q = c.bq_off[sub]; q < c.bq_off[sub + 1]; ++q) {
                bq += c.bq_w.at(q);
                surface_point(physical(m, e, reference(c.bq_xi, q)), ring, reference(c.bq_n, q));
            }
    }
    require(c.eb_points.size() % 9 == 0, "EB contains complete triangles");
    for (std::size_t t = 0; t < c.eb_points.size() / 9; ++t) {
        const auto e = c.eb_parent_elem.at(3 * t);
        if (m.attr_table.entries.at(m.attribute.at(e) - 1).body != lid)
            continue;
        const auto a = point(c.eb_points, 3 * t), b = point(c.eb_points, 3 * t + 1),
                   d = point(c.eb_points, 3 * t + 2);
        eb += norm(cross(minus(b, a), minus(d, a))) / 2;
        for (int v = 0; v < 3; ++v)
            surface_point(point(c.eb_points, 3 * t + v), ring, point(c.eb_normals, 3 * t + v));
    }
    near(sq, expected, 1e-12, "lid side -1 analytic area");
    near(bq, expected, 1e-12, "lid bq excludes internal caps");
    near(eb, expected, 1e-12, "lid EB excludes internal caps");
    const auto& p = *c.polyhedra;
    for (std::size_t poly = 0; poly < p.parent_elem_gid.size(); ++poly) {
        if (p.body.at(poly) != lid)
            continue;
        const auto parent =
            std::find(m.elem_gid.begin(), m.elem_gid.end(), p.parent_elem_gid[poly]);
        require(parent != m.elem_gid.end(), "exported parent exists");
        const auto [lo, size] = bounds(m, parent - m.elem_gid.begin());
        for (Index f = p.poly_off[poly]; f < p.poly_off[poly + 1]; ++f) {
            const auto face = p.poly_faces.at(f);
            const auto begin = p.face_off.at(face), end = p.face_off.at(face + 1);
            bool exterior = false;
            for (int axis = 0; axis < 3; ++axis)
                for (Real coordinate : {lo[axis], lo[axis] + size[axis]}) {
                    bool on_parent = true;
                    for (auto v = begin; v < end; ++v)
                        on_parent &= std::abs(point(p.points, p.face_conn.at(v))[axis] -
                                              coordinate) <= 1e-14;
                    exterior |= on_parent;
                }
            for (std::size_t edge = 0; edge < ring.size(); ++edge) {
                bool on_source = true;
                for (auto v = begin; v < end; ++v) {
                    const auto xyz = point(p.points, p.face_conn.at(v));
                    on_source &=
                        on_edge({xyz[0], xyz[1]}, ring[edge], ring[(edge + 1) % ring.size()]);
                }
                exterior |= on_source;
            }
            require(exterior, "exported boundary faces exclude decomposition caps");
        }
    }
}
void region_levels(const Output& out, const HexMeshArrays& original, const CutGeometry& g) {
    // The legacy entry emits level sets only for bodies appearing in planes.
    // Derive the oracle from the public constraints, including common planes
    // and unconstrained bodies, rather than assuming that legacy output is dense.
    std::set<BodyId> bodies;
    for (const auto& [name, body] : original.attr_table.bodies)
        bodies.insert(body);
    for (const auto& entry : original.attr_table.entries)
        bodies.insert(entry.body);
    for (const auto& plane : g.planes)
        if (plane.body != 0)
            bodies.insert(plane.body);
    for (const auto& ls : out.cut.level_sets) {
        require(bodies.erase(ls.body) == 1,
                "one level set for each constrained or registered body");
        const auto u = std::find_if(
            g.unions.begin(), g.unions.end(), [&](const auto& x) { return x.body == ls.body; });
        require(ls.phi.size() == out.mesh.node_gid.size(), "level set covers remapped nodes");
        for (std::size_t v = 0; v < out.mesh.node_gid.size(); ++v) {
            const auto p = point(out.mesh.vertices, v);
            Real want = -std::numeric_limits<Real>::infinity();
            for (const auto& plane : g.planes)
                if (plane.body == 0 || plane.body == ls.body)
                    want = std::max(want, (dot(plane.n, p) - plane.d) / norm(plane.n));
            if (u != g.unions.end())
                want = std::max(want, signed_distance({p[0], p[1]}, u->ring));
            const auto actual = ls.phi[v];
            check((std::isinf(want) && std::isinf(actual) &&
                   std::signbit(want) == std::signbit(actual)) ||
                      (std::isfinite(want) && std::isfinite(actual) &&
                       std::abs(actual - want) <= 1e-14),
                  "nodewise max(existing phi, signed region distance), body=" +
                      std::to_string(ls.body));
        }
    }
    require(bodies.empty(), "region union retains every body level set");
}
void volume_case(const Ring& ring, Real volume, Real embedded, bool producer) {
    near(area(ring), volume / .0005, 1e-12, "analytic source ring area");
    auto s = region_stack(ring);
    const auto original = sp5::grid(s, 8);
    const auto lid = original.attr_table.bodies.at("lid");
    auto g = producer ? value(region_cut_planes(s, original), "production geometry")
                      : CutGeometry{{}, {manual_union(lid, ring)}};
    auto a = applied(s, g, embedded < 1e-5 ? "L" : "U");
    const auto stats = value(cut_stats(a.mesh, a.cut), "body volume stats");
    near(stats.body_volume.at(lid), volume, 1e-12, "lid analytic volume");
    near(stats.body_volume.at(original.attr_table.bodies.at("substrate")),
         4e-7,
         1e-12,
         "unconstrained substrate volume");
    closure(a);
    surfaces(a, ring, lid, embedded);
    region_levels(a, original, g);
    auto b = applied(s, g, "repeat");
    exact(a.mesh, b.mesh, "same-process full mesh");
    exact(a.cut, b.cut, "same-process all cut fields");
    std::reverse(s.layers.back().regions[0].footprint.polygon.begin(),
                 s.layers.back().regions[0].footprint.polygon.end());
    auto reversed =
        producer
            ? value(region_cut_planes(s, original), "reverse region geometry")
            : CutGeometry{{}, {manual_union(lid, s.layers.back().regions[0].footprint.polygon)}};
    exact(g, reversed, "reverse geometry fields");
    auto r = applied(s, reversed, "reversed");
    exact(a.mesh, r.mesh, "reverse full mesh");
    exact(a.cut, r.cut, "reverse all cut fields");
}
void direct_case(bool immutable) {
    auto s = region_stack(l_ring());
    // Keep void candidates: tensor_grid already removes cells outside a source
    // region, which would make a nonempty removal-plan assertion vacuous.
    auto rectangular = s;
    rectangular.layers.back().regions[0].footprint.polygon.clear();
    auto original = sp5::grid(rectangular, 8);
    const auto lid = original.attr_table.bodies.at("lid");
    CutGeometry g{{}, {manual_union(lid, l_ring())}};
    auto a = direct(original, g, immutable);
    near(value(cut_stats(a.mesh, a.cut), "direct stats").body_volume.at(lid),
         1.72595e-7,
         1e-12,
         "both build_cut_layer overloads consume unions");
    closure(a);
    region_levels(a, original, g);
    if (immutable) {
        require(!a.cut.void_elem.empty(), "const union returns original-index removal plan");
        auto mutable_out = direct(original, g);
        std::set<Index> removed;
        for (auto e : a.cut.void_elem)
            removed.insert(original.elem_gid.at(e));
        for (auto gid : original.elem_gid)
            require(removed.contains(gid) == (std::find(mutable_out.mesh.elem_gid.begin(),
                                                        mutable_out.mesh.elem_gid.end(),
                                                        gid) == mutable_out.mesh.elem_gid.end()),
                    "const plan matches mutable void removal");
        for (std::size_t row = 0; row < a.cut.cut_elem.size(); ++row)
            require(a.cut.cut_elem_gid.at(row) == original.elem_gid.at(a.cut.cut_elem[row]),
                    "const cut element indices refer to original mesh");
    }
}
void forwarding(bool general, bool convex_outline) {
    auto s = region_stack(l_ring());
    auto original = sp5::grid(s, 8);
    const auto lid = original.attr_table.bodies.at("lid");
    if (convex_outline)
        s.outline.polygon = {{0, 0}, {.02, 0}, {.02, .02}, {0, .02}};
    CutGeometry g{{}, {manual_union(lid, l_ring())}};
    auto a = applied(s, g, "outline forwarding", general);
    near(value(cut_stats(a.mesh, a.cut), "forward stats").body_volume.at(lid),
         1.72595e-7,
         1e-12,
         "outline forwards region unions");
    closure(a);
}
void combined(bool producer) {
    const Ring region{{.0071, 0}, {.02, 0}, {.02, .02}, {0, .02}, {0, .0077}, {.0071, .0077}};
    auto s = region_stack(region);
    s.outline.polygon = l_ring();
    auto rectangular = s;
    rectangular.outline.polygon.clear();
    const auto original = sp5::grid(rectangular, 8);
    const auto lid = original.attr_table.bodies.at("lid");
    auto g = producer ? value(region_cut_planes(s, original), "combined geometry")
                      : CutGeometry{{}, {manual_union(lid, region)}};
    auto a = applied(s, g, "combined outline/region", true);
    const auto stats = value(cut_stats(a.mesh, a.cut), "combined stats");
    near(stats.body_volume.at(lid),
         1.4526e-7,
         1e-12,
         "nonconvex outline times nonconvex region intersection");
    near(stats.body_volume.at(original.attr_table.bodies.at("substrate")),
         3.4519e-7,
         1e-12,
         "body without a region union receives only the nonconvex outline");
    closure(a);
    for (const auto& ls : a.cut.level_sets)
        for (std::size_t v = 0; v < a.mesh.node_gid.size(); ++v) {
            const auto xyz = point(a.mesh.vertices, v);
            Real expected = signed_distance({xyz[0], xyz[1]}, s.outline.polygon);
            if (ls.body == lid)
                expected = std::max(expected, signed_distance({xyz[0], xyz[1]}, region));
            require(std::isfinite(ls.phi.at(v)) && std::abs(ls.phi[v] - expected) <= 1e-14,
                    "combined outline and body-specific region level set");
        }
}
void partition_identity() {
    const auto s = region_stack(l_ring());
    const auto original = sp5::grid(s, 8, 1);
    const auto g = value(region_cut_planes(s, original), "partition geometry");
    auto a = direct(original, g), b = direct(sp5::grid(s, 8, 2), g);
    partition_compare(a, b, original);
    for (const auto* out : {&a, &b})
        for (std::size_t f = 0; f < out->cut.gp_owner_rank.size(); ++f) {
            const auto gid = out->cut.gp_elem_gid[0].at(f);
            const auto i = std::find(out->mesh.elem_gid.begin(), out->mesh.elem_gid.end(), gid);
            require(i != out->mesh.elem_gid.end(), "ghost minus element survives");
            require(out->cut.gp_owner_rank[f] ==
                        out->mesh.partitioning.at(i - out->mesh.elem_gid.begin()),
                    "ghost owner follows rank of gid_minus");
        }
}
void exact_error(const Error& e, std::string_view path, std::string_view reason) {
    check(e.path == path && e.reason == reason,
          "expected " + std::string(path) + " / " + std::string(reason) + "; got " + e.path +
              " / " + e.reason);
}
void sentinel(HexMeshArrays& m) {
    const auto nan = std::bit_cast<Real>(std::uint64_t{0x7ff8000000000451ULL});
    m.copper_phi = {-0., nan};
    m.nodes = {-0., nan};
    m.copper_layer_of_layer = {-1};
    m.snapped.push_back({'x', -0., 0., -0.});
    m.grid_report.warnings.push_back("rollback sentinel");
}
void consumer_error(CutGeometry g, std::string_view path, std::string_view reason, int entry) {
    auto s = region_stack(l_ring());
    // Match the source and body display names for unambiguous I11 diagnostics.
    s.layers.back().regions[0].name = "lid";
    auto m = sp5::grid(s, 8);
    sentinel(m);
    const auto before = m;
    if (entry == 0 || entry == 1) {
        auto r = entry == 0 ? build_cut_layer(m, g, options())
                            : build_cut_layer(static_cast<const HexMeshArrays&>(m), g, options());
        require(!r, "invalid region union must fail build_cut_layer");
        exact_error(r.error(), path, reason);
    } else {
        auto r = entry == 2 ? apply_outline_cut(m, s.outline, options(), g)
                            : union_kernel::apply_outline_cut_general(m, s.outline, options(), g);
        require(!r, "invalid region union must fail outline entry");
        exact_error(r.error(), path, reason);
    }
    exact(m, before, "failed union leaves every caller field and NaN/signed-zero bit unchanged");
}
void invalid_union(int which, int entry) {
    const auto m = sp5::grid(region_stack(l_ring()), 8);
    auto u = manual_union(m.attr_table.bodies.at("lid"), l_ring());
    CutGeometry g;
    std::string path = "cut/union", reason = "invalid piece";
    if (which == 0 || which == 1) {
        g.unions = {u, u};
        path = "CUT_UNSUPPORTED";
        reason = "lid: duplicate piece union";
        // Duplicates win before either union's decomposition or ring validation.
        if (which == 1) {
            g.unions[0].ring = {{0, 0}};
            g.unions[0].pieces.clear();
        }
    } else if (which == 2) {
        u.internal.pop_back();
        g.unions = {u};
    } else if (which == 3) {
        u.internal[0].pop_back();
        g.unions = {u};
    } else if (which == 4 || which == 5) {
        u.pieces[0][0].body = which == 4 ? 0 : u.body + 101;
        g.unions = {u};
    } else {
        path = "CUT_UNSUPPORTED";
        if (which == 6)
            u.ring = {{0, 0}, {.01, 0}};
        if (which == 7)
            u.ring = {{0, 0}, {.01, 0}, {.02, 0}};
        if (which == 8)
            u.ring = {{0, 0}, {.02, .02}, {0, .02}, {.018, 0}};
        if (which == 9)
            u.ring = {{0, 0}, {.02, 0}, {.02, .02}, {.01, .01}, {0, .02}, {.01, .01}};
        if (which == 10) {
            u.pieces[0].clear();
            u.internal[0].clear();
        }
        if (which == 11)
            // Zero algebraic area takes precedence over self-intersection.
            u.ring = {{0, 0}, {.02, .02}, {0, .02}, {.02, 0}};
        if (which == 12) {
            u.pieces.clear();
            u.internal.clear();
        }
        if (which == 13) {
            // I11 rejects an empty block before I10 checks flag dimensions.
            u.pieces[0].clear();
            u.internal.pop_back();
        }
        reason =
            which == 8 || which == 9 ? "lid: self-intersecting polygon" : "lid: degenerate polygon";
        g.unions = {u};
    }
    consumer_error(g, path, reason, entry);
}
void producer_error(const Ring& ring, std::string_view reason, bool precedence) {
    auto s = region_stack(ring);
    s.layers.back().regions[0].name = "bad-region";
    if (precedence) {
        auto good = s.layers.back().regions[0];
        good.name = "first";
        good.footprint.polygon = square_ring();
        auto distinct = good;
        distinct.name = "second";
        distinct.footprint.polygon = l_ring();
        s.layers.back().regions.insert(s.layers.back().regions.begin(), {good, distinct});
    }
    const auto m = sp5b::registered_mesh();
    const auto before = m;
    const auto r = region_cut_planes(s, m);
    require(!r, "invalid source rejected before deduplication");
    exact_error(r.error(), "CUT_UNSUPPORTED", "bad-region: " + std::string(reason));
    exact(m, before, "producer leaves all mesh fields unchanged");
}
void source_dedupe(bool distinct) {
    auto s = region_stack(l_ring());
    auto upper = s.layers.back();
    upper.name = "upper";
    upper.regions[0].name = "same-body-upper";
    if (distinct)
        upper.regions[0].footprint.polygon = u_ring();
    else {
        auto& p = upper.regions[0].footprint.polygon;
        std::reverse(p.begin(), p.end());
        p.push_back(p.front());
        p.insert(p.begin(), p.front());
    }
    s.layers.push_back(upper);
    const auto m = sp5b::registered_mesh();
    const auto g = region_cut_planes(s, m);
    if (distinct) {
        require(!g, "distinct same-body nonconvex rings rejected");
        exact_error(g.error(), "CUT_UNSUPPORTED", "lid: body has multiple distinct cut polygons");
    } else {
        auto a = value(g, "identical normalized nonconvex rings dedupe");
        require(a.unions.size() == 1, "cross-layer duplicate source emits one union");
        exact(a.unions[0].ring, l_ring(), "deduplicated normalized ring");
    }
}
void empty_sources() {
    auto s = region_stack({});
    const auto m = sp5::grid(s, 8);
    auto g = value(region_cut_planes(s, m), "empty polygon source");
    require(g.planes.empty() && g.unions.empty(), "empty polygon never produces a union");
    s.layers.back().regions.clear();
    g = value(region_cut_planes(s, m), "no polygon source");
    require(g.planes.empty() && g.unions.empty(), "absent polygon never produces a union");
    const auto legacy = direct(m, CutGeometry{}, true);
    auto explicit_empty = CutGeometry{{}, {}};
    const auto actual = direct(m, explicit_empty, true);
    exact(legacy.mesh, actual.mesh, "empty-union mesh compatibility");
    exact(legacy.cut, actual.cut, "empty-union arrays compatibility");
}
void multiple_bodies(bool ordering_only) {
    auto s = region_stack(l_ring());
    auto upper = s.layers.back();
    upper.name = "upper";
    upper.regions[0].name = "aaa-region";
    upper.regions[0].body = "aaa";
    upper.regions[0].footprint.polygon = u_ring();
    s.layers.push_back(upper);
    auto m = sp5::grid(s, 8);
    if (ordering_only) {
        m.attr_table.bodies["lid"] = 17;
        m.attr_table.bodies["aaa"] = 91;
    }
    auto g = ordering_only ? value(region_cut_planes(s, m), "two independent target unions")
                           : CutGeometry{{},
                                         {manual_union(m.attr_table.bodies.at("aaa"), u_ring()),
                                          manual_union(m.attr_table.bodies.at("lid"), l_ring())}};
    require(g.unions.size() == 2, "two bodies produce two unions");
    if (ordering_only)
        require(g.unions[0].body < g.unions[1].body, "unions use numeric BodyId order");
    if (ordering_only) {
        require(g.unions[0].body == 17 && g.unions[1].body == 91,
                "numeric order beats source and label order");
        exact(g.unions[0].ring, l_ring(), "numeric order retains the lid ring");
        // Reverse the numeric assignment too: neither lexical nor source order
        // can satisfy both cases without sorting by the numeric key.
        m.attr_table.bodies["lid"] = 91;
        m.attr_table.bodies["aaa"] = 17;
        const auto reversed = value(region_cut_planes(s, m), "reassigned numeric bodies");
        require(reversed.unions.size() == 2 && reversed.unions[0].body == 17 &&
                    reversed.unions[1].body == 91,
                "reassigned numeric BodyIds determine output order");
        exact(reversed.unions[0].ring, u_ring(), "numeric order retains the other target ring");
        return;
    }
    const auto lid = m.attr_table.bodies.at("lid"), other = m.attr_table.bodies.at("aaa");
    g.planes = {{0, {0, 0, 1}, .0019},
                {lid, {0, 0, 1}, .0014},
                {other, {0, 0, 1}, .002},
                {lid + 1001, {1, 0, 0}, -10}};
    auto out = direct(m, g);
    const auto stats = value(cut_stats(out.mesh, out.cut), "body isolation stats");
    near(stats.body_volume.at(lid), 1.38076e-7, 1e-12, "lid union and body-filtered plane");
    near(stats.body_volume.at(other), 1.42136e-7, 1e-12, "second target U union and common plane");
    near(stats.body_volume.at(m.attr_table.bodies.at("substrate")),
         4e-7,
         1e-12,
         "unconstrained third body");
    region_levels(out, m, g);
    closure(out);
}
void rollback(const char* stage, int entry) {
    auto s = region_stack(l_ring());
    // Keep void candidates that tensor_grid normally removes using the region.
    auto rectangular = s;
    rectangular.layers.back().regions[0].footprint.polygon.clear();
    auto m = sp5::grid(rectangular, 8);
    const auto lid = m.attr_table.bodies.at("lid");
    // Put a lid void before a lid cut cell, so every seam observes both private
    // generated output and a pending removal before returning its late error.
    std::size_t void_e = m.elem_gid.size(), cut_e = void_e;
    for (std::size_t e = 0; e < m.elem_gid.size(); ++e) {
        if (m.attr_table.entries.at(m.attribute[e] - 1).body != lid)
            continue;
        auto [lo, size] = bounds(m, e);
        (void)size;
        if (lo[0] > .017 && lo[1] > .017)
            void_e = e;
        if (lo[0] > .014 && lo[0] < .016 && lo[1] > .011 && lo[1] < .014)
            cut_e = e;
    }
    require(void_e < m.elem_gid.size() && cut_e < m.elem_gid.size(), "late rollback fixture cells");
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
    m.pending_cut = {0, 1};
    sentinel(m);
    const auto before = m;
    CutGeometry g{{}, {manual_union(lid, l_ring())}};
    Fault fault(stage);
    const std::string path =
        std::string_view(stage) == "self_touching_face" ? "cut/union" : "cut/union/test";
    const std::string reason =
        std::string_view(stage) == "self_touching_face"
            ? "self-touching face ring after snap"
            : std::string("injected ") + stage + " after output and pending void";
    if (entry < 2) {
        auto r = entry == 0 ? build_cut_layer(m, g, options())
                            : build_cut_layer(static_cast<const HexMeshArrays&>(m), g, options());
        require(!r, "late union fault must reach real clipping/export/final validation");
        exact_error(r.error(), path, reason);
    } else {
        auto r = entry == 2 ? apply_outline_cut(m, s.outline, options(), g)
                            : union_kernel::apply_outline_cut_general(m, s.outline, options(), g);
        require(!r, "outline late union fault must propagate");
        exact_error(r.error(), path, reason);
    }
    exact(m, before, "late union failure rollback all fields");
}
void empty_domain(int entry) {
    auto s = region_stack(square_ring());
    auto rectangular = s;
    rectangular.layers.back().regions[0].footprint.polygon.clear();
    auto m = sp5::grid(rectangular, 8);
    sentinel(m);
    const auto before = m;
    auto u = manual_union(m.attr_table.bodies.at("lid"), square_ring());
    CutGeometry g{{{0, {0, 0, 1}, -1}}, {u}};
    auto r = entry == 0 ? apply_outline_cut(m, s.outline, options(), g)
                        : union_kernel::apply_outline_cut_general(m, s.outline, options(), g);
    require(!r, "empty domain reports error");
    exact_error(r.error(), "outline", "empty cut domain");
    exact(m, before, "empty resulting domain rolls back caller mesh");
}
// Read only the frozen array fields used here, so this test stays available
// without an optional JSON dependency. This is fixture IO, not a cut oracle.
struct FixtureArray {
    std::vector<FixtureArray> items;
    std::string atom;
};
FixtureArray fixture_array(std::istream& in) {
    in >> std::ws;
    FixtureArray result;
    if (in.peek() == '[') {
        in.get();
        in >> std::ws;
        while (in.peek() != ']') {
            result.items.push_back(fixture_array(in));
            in >> std::ws;
            if (in.peek() == ',') {
                in.get();
                in >> std::ws;
            } else
                require(in.peek() == ']', "fixture array separator");
        }
        in.get();
    } else if (in.peek() == '"') {
        in.get();
        std::getline(in, result.atom, '"');
    } else {
        while (in && in.peek() != ',' && in.peek() != ']' && !std::isspace(in.peek()))
            result.atom += static_cast<char>(in.get());
        require(!result.atom.empty(), "fixture scalar");
    }
    require(bool(in), "complete fixture array");
    return result;
}
FixtureArray fixture_field(const std::string& text, const std::string& name) {
    const auto key = text.find('"' + name + '"');
    require(key != std::string::npos, "fixture field exists");
    const auto colon = text.find(':', key);
    require(colon != std::string::npos, "fixture field colon");
    std::istringstream in(text.substr(colon + 1));
    return fixture_array(in);
}
Ring fixture_ring(const FixtureArray& a) {
    Ring r;
    for (const auto& v : a.items) {
        require(v.items.size() == 2, "fixture vertex dimension");
        r.push_back({std::stod(v.items[0].atom), std::stod(v.items[1].atom)});
    }
    return r;
}
PieceUnion fixture_union(BodyId body, bool split) {
    std::ifstream in(std::filesystem::path(CHIPCAE_SOURCE_DIR) /
                     "tests/contract/fixtures/cutgeom-nc-1.0/convex_split2/input.json");
    require(bool(in), "open repository I11 convex_split2 fixture");
    std::string text((std::istreambuf_iterator<char>(in)), {});
    PieceUnion u;
    u.body = body;
    u.ring = fixture_ring(fixture_field(text, "outline"));
    const auto blocks = fixture_field(text, split ? "pieces" : "single_piece");
    const auto flags = fixture_field(text, split ? "internal" : "single_internal");
    require(blocks.items.size() == flags.items.size(), "frozen I11 block dimensions");
    for (std::size_t k = 0; k < blocks.items.size(); ++k) {
        auto p = piece(fixture_ring(blocks.items[k]), u.ring);
        for (auto& plane : p.planes)
            plane.body = body;
        u.pieces.push_back(std::move(p.planes));
        u.internal.emplace_back();
        for (const auto& flag : flags.items[k].items) {
            require(flag.atom == "true" || flag.atom == "false", "fixture boolean");
            u.internal.back().push_back(flag.atom == "true");
        }
    }
    return u;
}
void equivalent(const CutLayerArrays& a, const CutLayerArrays& b, bool compare_levels = true) {
    // I11 requires the original array order as well as equal total measures.
    std::ostringstream counts;
    bool same = true;
    auto length = [&](std::size_t x, std::size_t y, const char* name) {
        if (x != y) {
            same = false;
            counts << ' ' << name << '=' << x << '/' << y;
        }
    };
    length(a.cut_elem.size(), b.cut_elem.size(), "cut_elem");
    length(a.sub_volfrac.size(), b.sub_volfrac.size(), "sub_volfrac");
    length(a.vq_w.size(), b.vq_w.size(), "vq_w");
    length(a.bq_w.size(), b.bq_w.size(), "bq_w");
    length(a.sq_w.size(), b.sq_w.size(), "sq_w");
    length(a.sq_side.size(), b.sq_side.size(), "sq_side");
    check(same, "split equivalence requires exact lengths/counts:" + counts.str());
    exact(a.sq_side, b.sq_side, "split surface sides in their original order");
    auto weights = [&](const auto& x, const auto& y, Real tolerance, const char* name) {
        require(x.size() == y.size(), "weight array lengths");
        for (std::size_t k = 0; k < x.size(); ++k)
            check(std::isfinite(x[k]) && std::isfinite(y[k]) &&
                      std::abs(x[k] - y[k]) <=
                          tolerance * std::max({Real(1), std::abs(x[k]), std::abs(y[k])}),
                  std::string("ordered split ") + name);
    };
    weights(a.sub_volfrac, b.sub_volfrac, 1e-14, "sub_volfrac");
    weights(a.sq_w, b.sq_w, 1e-14, "sq_w");
    weights(a.vq_w, b.vq_w, 1e-12, "vq_w");
    weights(a.bq_w, b.bq_w, 1e-12, "bq_w");
    auto fields_a = a, fields_b = b;
    if (!compare_levels) {
        // Legacy planes have plane-distance fields; I11 uses signed ring
        // distance and also emits unconstrained body fields. Check those
        // independently with region_levels instead of equating the two APIs.
        fields_a.level_sets.clear();
        fields_b.level_sets.clear();
    }
    Bits x, y;
    x.add(fields_a);
    y.add(fields_b);
    require(x.words.size() == y.words.size(), "all split cut-field lengths exactly equal");
    for (std::size_t k = 0; k < x.words.size(); ++k) {
        require(x.floating[k] == y.floating[k], "split field type");
        if (!x.floating[k])
            require(x.words[k] == y.words[k], "split integer/string/enum fields exactly equal");
        else {
            auto p = std::bit_cast<Real>(x.words[k]), q = std::bit_cast<Real>(y.words[k]);
            require((std::isinf(p) && x.words[k] == y.words[k]) ||
                        (std::isfinite(p) && std::isfinite(q) &&
                         std::abs(p - q) <= 1e-12 * std::max({Real(1), std::abs(p), std::abs(q)})),
                    "remaining ordered split floating fields");
        }
    }
}
void square_split() {
    auto s = region_stack(square_ring());
    auto rectangular = s;
    rectangular.layers.back().regions[0].footprint.polygon.clear();
    const auto m = sp5::grid(rectangular, 8);
    const auto g = value(region_cut_planes(s, m), "legacy convex square");
    require(g.unions.empty(), "convex square retains legacy plane path");
    const CutGeometry split{{}, {split_square(m.attr_table.bodies.at("lid"))}};
    const auto a = direct(m, g), b = direct(m, split);
    equivalent(a.cut, b.cut, false);
    region_levels(b, m, split);
}
void fixture_split() {
    auto m = sp5::grid(stack(), 8);
    const auto body = m.attr_table.bodies.at("substrate");
    const CutGeometry single{{}, {fixture_union(body, false)}},
        split{{}, {fixture_union(body, true)}};
    const auto a = direct(m, single), b = direct(m, split);
    require(!a.cut.cut_elem.empty(), "I11 fixture single union performs cutting");
    equivalent(a.cut, b.cut);
    region_levels(a, m, single);
    region_levels(b, m, split);
}
void subdivided_square_split(bool immutable) {
    // Redundant collinear exterior vertices must not survive canonicalization
    // as extra quadrature faces. Plane magnitudes do not change half-spaces.
    auto s = region_stack({});
    const auto m = sp5::grid(s, 8);
    const auto body = m.attr_table.bodies.at("lid");
    auto split = split_square(body);
    const Ring left{
        {.004, .004}, {.007, .004}, {.0101, .004}, {.0101, .016}, {.007, .016}, {.004, .016}};
    auto p = piece(left, split.ring);
    for (auto& plane : p.planes) {
        plane.body = body;
        for (auto& n : plane.n)
            n *= 2;
        plane.d *= 2;
    }
    split.pieces[0] = std::move(p.planes);
    split.internal[0] = std::move(p.internal);
    const CutGeometry single{{}, {manual_union(body, square_ring())}}, split_geometry{{}, {split}};
    const auto a = direct(m, single, immutable), b = direct(m, split_geometry, immutable);
    equivalent(a.cut, b.cut);
    exact(a.mesh, b.mesh, "collinear split preserves mesh fields");
    region_levels(b, m, split_geometry);
}
// Three strips include a pair with an empty feasible intersection. Adding a
// redundant, almost parallel plane must not prevent convex certification.
void separated_strip_split(bool near_parallel, bool immutable) {
    const auto m = sp5::grid(region_stack({}), 8);
    const auto body = m.attr_table.bodies.at("lid");
    PieceUnion split;
    split.body = body;
    split.ring = square_ring();
    const std::array<Real, 4> x{.004, .008, .012, .016};
    for (std::size_t k = 0; k + 1 < x.size(); ++k) {
        const Ring block{{x[k], .004}, {x[k + 1], .004}, {x[k + 1], .016}, {x[k], .016}};
        auto p = piece(block, split.ring);
        for (auto& plane : p.planes)
            plane.body = body;
        split.pieces.push_back(std::move(p.planes));
        split.internal.push_back(std::move(p.internal));
    }
    if (near_parallel) {
        split.pieces.front().push_back({body, {1, 1e-20, 0}, .016});
        split.internal.front().push_back(false);
    }
    const auto a = direct(m, CutGeometry{{}, {manual_union(body, square_ring())}}, immutable);
    const auto b = direct(m, CutGeometry{{}, {split}}, immutable);
    equivalent(a.cut, b.cut);
    exact(a.mesh, b.mesh, "separated strips preserve mesh fields");
    closure(b);
}

void zero_coordinate_split(bool immutable) {
    const auto m = sp5::grid(region_stack({}), 8);
    const auto body = m.attr_table.bodies.at("lid");
    // The first triangle touches x=0 at one vertex without carrying that
    // exterior plane. Reconstructing the vertex can leave a tiny negative x;
    // a tolerance based only on the x-plane's evaluated terms vanishes there.
    const Ring ring{{0, .0001}, {.017, .006}, {.015, .018}, {0, .0144}};
    PieceUnion split;
    split.body = body;
    split.ring = ring;
    for (const Ring& block :
         std::vector<Ring>{{ring[0], ring[1], ring[2]}, {ring[0], ring[2], ring[3]}}) {
        auto p = piece(block, ring);
        for (auto& plane : p.planes)
            plane.body = body;
        split.pieces.push_back(std::move(p.planes));
        split.internal.push_back(std::move(p.internal));
    }
    const auto a = direct(m, CutGeometry{{}, {manual_union(body, ring)}}, immutable);
    const auto b = direct(m, CutGeometry{{}, {split}}, immutable);
    equivalent(a.cut, b.cut);
    exact(a.mesh, b.mesh, "zero-coordinate split preserves mesh fields");
    closure(b);
}

void entry_only_level_sets(bool immutable) {
    auto m = sp5::grid(region_stack({}), 8);
    const auto lid = m.attr_table.bodies.at("lid");
    m.attr_table.bodies.erase("substrate");
    // Duplicate registry values and repeated entries must still yield one field.
    m.attr_table.bodies["lid_alias"] = lid;
    auto extra = m.attr_table.entries.front();
    extra.body = 91;
    m.attr_table.entries.push_back(extra);
    m.attr_table.entries.push_back(extra);
    const CutGeometry g{{{lid, {0, 0, 1}, .0014}, {91, {1, 0, 0}, .01}},
                        {manual_union(lid, l_ring())}};
    region_levels(direct(m, g, immutable), m, g);
}

void plane_only_level_sets(bool immutable) {
    const auto m = sp5::grid(region_stack({}), 8);
    const auto lid = m.attr_table.bodies.at("lid");
    // Neither plane-only body is in the registry or any attribute entry.
    // Include repeated planes and a common constraint to check merging/filtering.
    const CutGeometry g{{{91, {2, 0, 0}, .02},
                         {0, {0, 0, 2}, .0029},
                         {91, {0, 4, 0}, .048},
                         {90, {-3, 0, 0}, -.012}},
                        {manual_union(lid, l_ring())}};
    const auto out = direct(m, g, immutable);
    region_levels(out, m, g);
    for (std::size_t i = 1; i < out.cut.level_sets.size(); ++i)
        require(out.cut.level_sets[i - 1].body < out.cut.level_sets[i].body,
                "plane-only fields merge in strictly increasing BodyId order");
}

void union_outline_phi_mismatch(bool longer) {
    auto m = sp5::grid(region_stack({}), 8);
    sentinel(m);
    const auto before = m;
    const CutGeometry g{{}, {manual_union(m.attr_table.bodies.at("lid"), l_ring())}};
    const std::vector<union_kernel::UnionPiece> outline{piece(square_ring(), square_ring())};
    const auto nodes = m.vertices.size() / 3;
    const std::vector<Real> phi(longer ? nodes + 1 : nodes - 1, -1);
    const auto result = union_kernel::build_cut_layer_union(m, outline, g, options(), phi);
    require(!result, "body unions reject short and long outline fields before indexing");
    exact_error(result.error(), "cut/union", "outline_phi size mismatch");
    exact(m, before, "outline field mismatch preserves every caller field");
}

void union_options_precedence(bool immutable, int which) {
    auto m = sp5::grid(region_stack({}), 8);
    sentinel(m);
    const auto before = m;
    const auto u = manual_union(m.attr_table.bodies.at("lid"), l_ring());
    auto o = options();
    if (which == 0)
        o.snap_phi = -1;
    else if (which == 1)
        o.snap_phi = std::numeric_limits<Real>::quiet_NaN();
    else if (which == 2)
        o.small_sub_vf = -1;
    else if (which == 3)
        o.small_sub_vf = 1;
    else if (which == 4)
        o.small_sub_vf = std::numeric_limits<Real>::quiet_NaN();
    else
        o.quad_degree = 3;
    std::array<CutGeometry, 4> invalid{
        CutGeometry{{}, {u, u}}, CutGeometry{{}, {u}}, CutGeometry{{}, {u}}, CutGeometry{{}, {u}}};
    invalid[1].unions[0].ring = {{0, 0}};
    invalid[2].unions[0].internal.clear();
    invalid[3].unions[0].pieces[0][0].n = {0, 0, 0};
    for (const auto& g : invalid) {
        const auto result = immutable ? build_cut_layer(static_cast<const HexMeshArrays&>(m), g, o)
                                      : build_cut_layer(m, g, o);
        require(!result, "invalid options and union must fail");
        exact_error(result.error(), "cut/options", "invalid thresholds");
        exact(m, before, "invalid options preserve every caller field");
    }
}

void legacy_convex_cleanup(bool reverse) {
    const auto m = sp5b::registered_mesh();
    // Legacy cleanup removes a collinear out-and-back excursion from a square
    // and a repeated diagonal excursion from a triangle before testing convexity.
    const std::array<Ring, 2> raw{
        Ring{{.004, .004},
             {.016, .004},
             {.016, .016},
             {.01, .016},
             {.01, .018},
             {.01, .016},
             {.004, .016}},
        Ring{{0, 0}, {.02, 0}, {.02, .02}, {.01, .01}, {0, .02}, {.01, .01}}};
    const std::array<Ring, 2> cleaned{square_ring(), Ring{{0, 0}, {.02, 0}, {.02, .02}}};
    for (std::size_t k = 0; k < raw.size(); ++k) {
        auto s = region_stack(cleaned[k]);
        const auto expected = value(region_cut_planes(s, m), "clean convex reference");
        auto ring = raw[k];
        if (reverse)
            std::reverse(ring.begin(), ring.end());
        s.layers.back().regions[0].footprint.polygon = ring;
        const auto actual = value(region_cut_planes(s, m), "legacy convex cleanup");
        require(actual.unions.empty(), "legacy convex cleanup retains the planes path");
        exact(actual, expected, "cleaned convex planes retain every bit and their order");
    }
}

void balanced_overlap_rejected(bool immutable) {
    auto m = sp5::grid(stack(), 8);
    const auto before = m;
    const auto body = m.attr_table.bodies.at("substrate");
    PieceUnion u;
    u.body = body;
    u.ring = square_ring();
    // The overlap [.008,.009] exactly balances the gap [.010,.011].
    // Containment and summed area alone would incorrectly fill the square.
    for (const auto& x : std::array<P2, 3>{{{.004, .009}, {.008, .010}, {.011, .016}}}) {
        auto p = piece({{x[0], .004}, {x[1], .004}, {x[1], .016}, {x[0], .016}}, u.ring);
        for (auto& plane : p.planes)
            plane.body = body;
        u.pieces.push_back(std::move(p.planes));
        u.internal.push_back(std::move(p.internal));
    }
    const auto result =
        immutable
            ? build_cut_layer(static_cast<const HexMeshArrays&>(m), CutGeometry{{}, {u}}, options())
            : build_cut_layer(m, CutGeometry{{}, {u}}, options());
    require(!result, "balanced overlap/gap cannot certify a convex union");
    exact_error(result.error(), "outline.unsupported", "outline: disconnected solid in one cell");
    exact(m, before, "overlap rejection preserves every mesh field");
}

void supplied_piece_geometry(bool convex_pieces, int entry) {
    // Deliberately give diagnostics a different valid ring. Replacing supplied
    // half-spaces by that ring (or its convex hull) changes the physical solid.
    const auto s = region_stack({});
    const auto m = sp5::grid(s, 8);
    const auto lid = m.attr_table.bodies.at("lid");
    const auto physical_ring = convex_pieces ? square_ring() : l_ring();
    auto u = convex_pieces ? split_square(lid) : manual_union(lid, physical_ring);
    u.ring = convex_pieces ? l_ring() : square_ring();
    const CutGeometry g{{}, {u}};
    const auto out =
        entry < 2 ? direct(m, g, entry == 1) : applied(s, g, "supplied piece geometry", entry == 3);
    const auto stats = value(cut_stats(out.mesh, out.cut), "supplied piece stats");
    near(stats.body_volume.at(lid),
         convex_pieces ? 7.2e-8 : 1.72595e-7,
         1e-12,
         "only piece planes determine the target solid");
    near(stats.body_volume.at(m.attr_table.bodies.at("substrate")),
         4e-7,
         1e-12,
         "diagnostic ring does not constrain another body");
    closure(out);
    surfaces(out, physical_ring, lid, convex_pieces ? 2.4e-5 : 7.5e-6);
    region_levels(out, m, g);
}
void i10_split_geometry() {
    // Frozen I10 integrates pieces separately. Its geometric measures and
    // boundary invariants remain required; ordered raw-array identity is I11's
    // additional requirement, exercised above without changing I10 semantics.
    const auto m = sp5::grid(stack(), 8);
    const auto body = m.attr_table.bodies.at("substrate");
    auto kernel = [&](bool split) {
        auto copy = m;
        const auto u = fixture_union(0, split);
        std::vector<union_kernel::UnionPiece> ps;
        for (std::size_t k = 0; k < u.pieces.size(); ++k)
            ps.push_back({u.pieces[k], u.internal[k]});
        auto c = value(union_kernel::build_cut_layer_union(
                           copy, ps, {}, options(), std::vector<Real>(copy.node_gid.size(), 0)),
                       "I10 split geometry");
        return Output{std::move(copy), std::move(c)};
    };
    const auto single = kernel(false), split = kernel(true);
    exact(single.mesh, split.mesh, "I10 split mesh and survivor order");
    exact(single.cut.cut_elem_gid, split.cut.cut_elem_gid, "I10 split cut-cell order");
    exact(single.cut.sub_off, split.cut.sub_off, "I10 split subcell counts");
    require(single.cut.sub_volfrac.size() == split.cut.sub_volfrac.size(),
            "I10 split fraction counts");
    for (std::size_t k = 0; k < single.cut.sub_volfrac.size(); ++k)
        require(std::abs(single.cut.sub_volfrac[k] - split.cut.sub_volfrac[k]) <= 1e-14,
                "I10 split summed subcell fractions");
    for (const auto* out : {&single, &split}) {
        near(value(cut_stats(out->mesh, out->cut), "I10 split stats").body_volume.at(body),
             2.55e-7,
             1e-12,
             "I10 analytic rectangular volume");
        closure(*out);
        surfaces(*out, fixture_union(0, false).ring, body, 6.4e-5);
    }
}
void small_sub_sum() {
    // Two disjoint-interior strips join on y=.0625. Each piece contributes
    // 1/16, while the cell union contributes exactly 1/8; thresholding occurs once.
    auto s = stack();
    s.outline.box = {0, 0, 8, 8};
    s.layers[0].thickness = 2;
    auto m = sp5::grid(s, 8);
    const auto body = m.attr_table.bodies.at("substrate");
    PieceUnion u;
    u.body = body;
    u.ring = {{0, 0}, {8, 0}, {8, .125}, {0, .125}};
    for (const Ring& r : std::vector<Ring>{{{0, 0}, {8, 0}, {8, .0625}, {0, .0625}},
                                           {{0, .0625}, {8, .0625}, {8, .125}, {0, .125}}}) {
        auto p = piece(r, u.ring);
        for (auto& plane : p.planes)
            plane.body = body;
        u.pieces.push_back(p.planes);
        u.internal.push_back(p.internal);
    }
    auto o = options();
    o.small_sub_vf = .09375;
    const auto c = value(build_cut_layer(m, CutGeometry{{}, {u}}, o), "union-volume small_subs");
    require(c.cut_elem.size() == 16 && c.sub_volfrac.size() == 16,
            "one subcell per union cut cell");
    require(c.small_subs.empty(), "sum pieces before applying small_subs threshold once");
    for (auto fraction : c.sub_volfrac)
        exact(fraction, .125, "binary exact summed piece fraction");
}
void timings() {
    std::array<Real, 3> seconds{};
    bool all = true;
    const std::array<Ring, 3> rings{l_ring(), u_ring(), square_ring()};
    for (std::size_t k = 0; k < rings.size(); ++k) {
        auto s = region_stack(rings[k]);
        auto rectangular = s;
        rectangular.layers.back().regions[0].footprint.polygon.clear();
        auto m = sp5::grid(rectangular, 8);
        const auto g =
            k == 2 ? value(region_cut_planes(s, m), "convex timing geometry")
                   : CutGeometry{{}, {manual_union(m.attr_table.bodies.at("lid"), rings[k])}};
        const auto start = std::chrono::steady_clock::now();
        const auto r = apply_outline_cut(m, s.outline, options(), g);
        seconds[k] = std::chrono::duration<Real>(std::chrono::steady_clock::now() - start).count();
        all &= bool(r) && r.value().has_value();
        std::cout << "MEASURE "
                  << (k == 0   ? "L"
                      : k == 1 ? "U"
                               : "convex")
                  << " seconds=" << seconds[k] << " peak_rss_MiB=" << rss_mib() << '\n';
    }
    std::cout << "MEASURE L/convex=" << seconds[0] / seconds[2]
              << " U/convex=" << seconds[1] / seconds[2] << " (no thresholds)\n";
    require(all, "timed L/U/convex calls must all return cut layers");
}

} // namespace
int main(int argc, char** argv) {
    int failures = 0, cases = 0;
    const auto harness = sp5::run([&] {
        auto run = [&](const std::string& name, const std::function<void()>& test) {
            if (argc > 1 && name != argv[1])
                return;
            std::cout << "RUN " << name << std::endl;
            try {
                test();
            } catch (const std::exception& e) {
                std::cerr << "FAIL " << name << ": " << e.what() << '\n';
                ++failures;
            }
            ++cases;
        };
        run("convex_baseline", [] {
            const auto* dir = std::getenv("CHIPCAE_REGION_BASELINE_DIR");
            baseline(dir ? dir : "");
        });
        run("L_production", [] { production(l_ring()); });
        run("U_production", [] { production(u_ring()); });
        run("L_production_to_cut", [] { volume_case(l_ring(), 1.72595e-7, 7.5e-6, true); });
        run("U_production_to_cut", [] { volume_case(u_ring(), 1.7767e-7, 1.06e-5, true); });
        run("L_union_consumption", [] { volume_case(l_ring(), 1.72595e-7, 7.5e-6, false); });
        run("U_union_consumption", [] { volume_case(u_ring(), 1.7767e-7, 1.06e-5, false); });
        for (bool immutable : {false, true})
            run(immutable ? "direct_const" : "direct_mutable", [=] { direct_case(immutable); });
        for (bool general : {false, true})
            for (bool convex : {false, true})
                run("forward_" + std::to_string(general) + "_" + std::to_string(convex),
                    [=] { forwarding(general, convex); });
        run("combined_outline_region", [] { combined(true); });
        run("combined_union_consumption", [] { combined(false); });
        run("nodewise_region_level_sets", [] {
            auto m = sp5::grid(region_stack(l_ring()), 8);
            const auto lid = m.attr_table.bodies.at("lid");
            CutGeometry g{{{lid, {0, 0, 1}, .0014}}, {manual_union(lid, l_ring())}};
            region_levels(direct(m, g), m, g);
        });
        run("nodewise_common_and_nonunit_planes", [] {
            auto m = sp5::grid(region_stack(l_ring()), 8);
            const auto lid = m.attr_table.bodies.at("lid");
            const CutGeometry g{
                {{0, {0, 0, 2}, .0029}, {lid, {0, 0, 4}, .0056}, {lid + 101, {1, 0, 0}, -10}},
                {manual_union(lid, l_ring())}};
            for (bool immutable : {false, true})
                region_levels(direct(m, g, immutable), m, g);
        });
        run("partition_L", partition_identity);
        run("body_isolation", [] { multiple_bodies(false); });
        run("numeric_body_order", [] { multiple_bodies(true); });
        run("square_split_equivalence", square_split);
        run("I11_convex_split2", fixture_split);
        for (bool immutable : {false, true})
            run("I11_collinear_split_" + std::to_string(immutable),
                [=] { subdivided_square_split(immutable); });
        for (bool immutable : {false, true}) {
            run("zero_coordinate_split_" + std::to_string(immutable),
                [=] { zero_coordinate_split(immutable); });
            run("entry_only_level_sets_" + std::to_string(immutable),
                [=] { entry_only_level_sets(immutable); });
            run("plane_only_level_sets_" + std::to_string(immutable),
                [=] { plane_only_level_sets(immutable); });
            for (int which = 0; which < 6; ++which)
                run("union_options_precedence_" + std::to_string(immutable) + "_" +
                        std::to_string(which),
                    [=] { union_options_precedence(immutable, which); });
            run("balanced_overlap_rejected_" + std::to_string(immutable),
                [=] { balanced_overlap_rejected(immutable); });
            for (bool near_parallel : {false, true})
                run("separated_strip_split_" + std::to_string(near_parallel) + "_" +
                        std::to_string(immutable),
                    [=] { separated_strip_split(near_parallel, immutable); });
        }
        for (bool convex : {false, true})
            for (int entry = 0; entry < 4; ++entry)
                run("piece_geometry_" + std::to_string(convex) + "_entry_" + std::to_string(entry),
                    [=] { supplied_piece_geometry(convex, entry); });
        run("I10_split_geometry", i10_split_geometry);
        run("small_subs_sum_once", small_sub_sum);
        run("source_dedupe", [] { source_dedupe(false); });
        run("source_distinct", [] { source_dedupe(true); });
        run("empty_sources", empty_sources);
        for (bool variant : {false, true}) {
            run("union_outline_phi_mismatch_" + std::to_string(variant),
                [=] { union_outline_phi_mismatch(variant); });
            run("legacy_convex_cleanup_" + std::to_string(variant),
                [=] { legacy_convex_cleanup(variant); });
        }
        const std::array<Real, 3> nonfinite{std::numeric_limits<Real>::quiet_NaN(),
                                            std::numeric_limits<Real>::infinity(),
                                            -std::numeric_limits<Real>::infinity()};
        const std::array<Ring, 2> finite_rings{
            square_ring(), Ring{{.004, .01}, {.01, .004}, {.016, .01}, {.01, .016}}};
        for (std::size_t shape = 0; shape < finite_rings.size(); ++shape)
            for (std::size_t k = 0; k < nonfinite.size(); ++k)
                for (std::size_t vertex = 0; vertex < finite_rings[shape].size(); ++vertex)
                    for (int axis = 0; axis < 2; ++axis)
                        run("nonfinite_source_" + std::to_string(shape) + "_" + std::to_string(k) +
                                "_" + std::to_string(vertex) + "_" + std::to_string(axis),
                            [&, shape, k, vertex, axis] {
                                auto ring = finite_rings[shape];
                                ring[vertex][axis] = nonfinite[k];
                                producer_error(ring, "degenerate polygon", false);
                            });
        const std::vector<Ring> bad{{{0, 0}, {.01, 0}},
                                    {{0, 0}, {.01, 0}, {.02, 0}},
                                    {{0, 0}, {.02, .02}, {0, .02}, {.018, 0}},
                                    {{0, 0},
                                     {.02, 0},
                                     {.02, .02},
                                     {.012, .02},
                                     {.012, .012},
                                     {.008, .012},
                                     {.008, .02},
                                     {0, .02},
                                     {0, .01},
                                     {.004, .01},
                                     {.004, .014},
                                     {.004, .01},
                                     {0, .01}},
                                    {{0, 0}, {.02, .02}, {0, .02}, {.02, 0}}};
        for (std::size_t k = 0; k < bad.size(); ++k)
            for (bool precedence : {false, true})
                run("source_error_" + std::to_string(k) + "_" + std::to_string(precedence),
                    [&, k, precedence] {
                        producer_error(bad[k],
                                       k == 2 || k == 3 ? "self-intersecting polygon"
                                                        : "degenerate polygon",
                                       precedence);
                    });
        for (int which = 0; which <= 13; ++which)
            for (int entry = 0; entry < 4; ++entry)
                run("union_error_" + std::to_string(which) + "_entry_" + std::to_string(entry),
                    [=] { invalid_union(which, entry); });
        for (const char* stage : {"clipping", "export", "final_validation", "self_touching_face"})
            for (int entry = 0; entry < 4; ++entry) {
                // Const generation must never enter the remapping/finalization stage.
                if (entry == 1 && std::string_view(stage) == "final_validation")
                    continue;
                run(std::string("rollback_") + stage + "_entry_" + std::to_string(entry),
                    [=] { rollback(stage, entry); });
            }
        run("const_never_finalizes", [] {
            auto s = region_stack(l_ring());
            s.layers.back().regions[0].footprint.polygon.clear();
            auto m = sp5::grid(s, 8);
            const auto before = m;
            const CutGeometry g{{}, {manual_union(m.attr_table.bodies.at("lid"), l_ring())}};
            Fault fault("final_validation");
            const auto c =
                value(build_cut_layer(static_cast<const HexMeshArrays&>(m), g, options()),
                      "const generation bypasses finalize");
            require(!c.void_elem.empty(), "const returns original-index void plan");
            exact(m, before, "const never remaps caller fields");
            for (const auto& ls : c.level_sets)
                require(ls.phi.size() == before.node_gid.size(),
                        "const keeps original node indexing");
        });
        for (int entry = 0; entry < 2; ++entry)
            run("empty_domain_" + std::to_string(entry), [=] { empty_domain(entry); });
        run("timings", timings);
        require(cases > 0, "test filter selects a case");
        std::cout << cases << " cases, " << failures << " failed\n";
    });
    return harness == 0 && failures == 0 ? 0 : 1;
}
