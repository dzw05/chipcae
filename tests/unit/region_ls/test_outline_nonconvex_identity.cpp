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

HexMeshArrays demo_background(const StackModel& stack) {
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

Ring octagon() {
    return {{.003, 0},
            {.017, 0},
            {.02, .003},
            {.02, .017},
            {.017, .02},
            {.003, .02},
            {0, .017},
            {0, .003}};
}
Footprint footprint(const Ring& ring) {
    return {{0, 0, .02, .02}, ring};
}
HexMeshArrays background() {
    auto s = stack();
    s.outline.polygon.clear();
    auto m = support::grid(s, 8);
    require(m.elem_gid.size() == 128 && m.node_gid.size() == 243,
            "S fixture must have 128 elements and 243 nodes");
    return m;
}
struct Output {
    HexMeshArrays mesh;
    std::optional<CutLayerArrays> cut;
};
Output compare_paths(const HexMeshArrays& original,
                     const Footprint& outline,
                     const CutOptions& options = {},
                     const CutGeometry& extra = {}) {
    auto legacy_mesh = original, general_mesh = original;
    const auto legacy = value(apply_outline_cut(legacy_mesh, outline, options, extra),
                              "legacy four-argument outline cut");
    auto actual = value(general::apply_outline_cut_general(general_mesh, outline, options, extra),
                        "general outline cut");
    exact(general_mesh, legacy_mesh, "convex identity: every mesh field");
    require(actual.has_value() == legacy.has_value(), "convex identity: optional presence");
    if (actual)
        exact(*actual, *legacy, "convex identity: every cut field");
    return {std::move(general_mesh), std::move(actual)};
}
void convex_bitwise_octagon() {
    auto m = background();
    require(m.axes[0].size() == 9 && m.axes[1].size() == 9 && m.axes[2].size() == 3,
            "octagon uses rectangular 8 x 8 x 2 background");
    // A registry whose name order differs from BodyId order catches plane
    // reconstruction or a different body iteration order in the new entry.
    m.attr_table.bodies["aaa-unused"] = 73;
    const auto original = m;
    for (int degree : {2, 4, 6, 8}) {
        CutOptions options;
        options.quad_degree = degree;
        options.export_polyhedra = false;
        options.keep_void = true;
        auto out = compare_paths(m, footprint(octagon()), options);
        require(out.cut.has_value() && out.cut->polyhedra.has_value(),
                "outline entry forces polyhedron export");
        require(out.cut->void_elem.empty(), "outline entry removes voids despite keep_void=true");
        auto reversed = octagon();
        std::reverse(reversed.begin(), reversed.end());
        compare_paths(m, footprint(reversed), options);
        auto rotated = octagon();
        std::rotate(rotated.begin(), rotated.begin() + 3, rotated.end());
        compare_paths(m, footprint(rotated), options);
        auto closed = octagon();
        closed.push_back(closed.front());
        compare_paths(m, footprint(closed), options);
        const Real z0 = m.axes[2].front();
        compare_paths(
            m, footprint(octagon()), options, CutGeometry{{Plane{0, {0, 0, 2}, 2 * (z0 + .0007)}}});
    }
    exact(m, original, "fresh copies leave the identity fixture untouched");
}
void convex_bitwise_demo_gate() {
    const auto s = fixture();
    const auto m = demo_background(s);
    require(m.elem_gid.size() == 1734 && m.node_gid.size() == 2268,
            "copied DEMO-GATE background counts");
    auto out = compare_paths(m, s.outline);
    require(out.cut.has_value(), "DEMO-GATE produces cut output");
    require(out.mesh.elem_gid.size() == 1728 && out.mesh.node_gid.size() == 2261 &&
                out.cut->cut_elem.size() == 66,
            "DEMO-GATE remains 1728 / 2261 / cut 66");
    const auto digest = chipcae::preview::detail::digest(out.mesh);
    require(digest.sha256 == "efc79a629779602538239635706a9f4293e897f8e46b3c2aeb43c22c6b6b9f4e",
            "DEMO-GATE frozen mesh digest");
    near(value(cut_stats(out.mesh, *out.cut), "DEMO-GATE stats").solid_volume,
         6.272e-7,
         1e-10,
         "DEMO-GATE analytic volume");
    std::cout << "convex DEMO-GATE full-field identity: 1728 / 2261 / cut 66, digest="
              << digest.sha256 << '\n';
}
void convex_bitwise_collinear() {
    const auto original = octagon();
    Ring subdivided;
    for (std::size_t i = 0; i < original.size(); ++i) {
        const auto a = original[i], b = original[(i + 1) % original.size()];
        subdivided.push_back(a);
        subdivided.push_back({a[0] + .375 * (b[0] - a[0]), a[1] + .375 * (b[1] - a[1])});
    }
    // Legacy accepts these subdivisions; cleanup would discard their planes.
    // Compare every output field against legacy using the original vertices.
    const auto m = background();
    compare_paths(m, footprint(subdivided));
    std::reverse(subdivided.begin(), subdivided.end());
    std::rotate(subdivided.begin(), subdivided.begin() + 3, subdivided.end());
    subdivided.push_back(subdivided.front());
    compare_paths(m, footprint(subdivided));
}
void empty_nullopt() {
    const auto m = background();
    const auto out = compare_paths(m, footprint({}));
    require(!out.cut, "empty outline and empty extra must return nullopt");
    exact(out.mesh, m, "empty nullopt leaves every mesh field unchanged");
    const auto extra = CutGeometry{{Plane{0, {1, 0, 0}, .0137}}};
    require(compare_paths(m, footprint({}), {}, extra).cut.has_value(),
            "empty polygon delegates nonempty extra to legacy");
}
void duplicate_cleanup_convex() {
    auto dirty = octagon();
    dirty.insert(dirty.begin() + 1, {.01, 0});
    dirty.insert(dirty.begin() + 2, {.01, 0});
    dirty.push_back(dirty.front());
    const auto m = background();
    auto legacy_mesh = m;
    const auto legacy_error = apply_outline_cut(legacy_mesh, footprint(dirty), {}, {});
    require(!legacy_error, "legacy still rejects consecutive duplicate vertices");
    require(legacy_error.error().path == "outline.unsupported" &&
                legacy_error.error().reason == "duplicate outline vertex",
            "legacy duplicate error remains literal");
    exact(legacy_mesh, m, "legacy duplicate validation leaves mesh unchanged");
    auto expected_mesh = m, actual_mesh = m;
    const auto expected = value(apply_outline_cut(expected_mesh, footprint(octagon()), {}, {}),
                                "cleaned convex legacy oracle");
    const auto actual =
        value(general::apply_outline_cut_general(actual_mesh, footprint(dirty), {}, {}),
              "duplicate/collinear cleanup must reach convex path");
    require(actual.has_value() && expected.has_value(), "cleaned convex produces cut arrays");
    exact(actual_mesh, expected_mesh, "cleaned convex full mesh identity");
    exact(*actual, *expected, "cleaned convex full cut identity");
}
void rollback_sentinels(HexMeshArrays& m) {
    m.copper_phi = {-0., .125};
    m.copper_layer_of_layer = {-1};
    m.nodes = {-0., .25};
    m.snapped.push_back({'x', -0., 0., -0.});
    m.grid_report.warnings.push_back("synthetic rollback sentinel");
    m.preparation_report.push_back({"rollback", {"substrate"}, {"void"}, 1, 2, .25, -0.});
    m.pending_cut = {0, 1};
}
void failure(const Ring& ring,
             std::string_view path,
             std::string_view reason,
             const CutOptions& options = {},
             const CutGeometry& extra = {}) {
    auto m = background();
    rollback_sentinels(m);
    const auto original = m;
    const auto result = general::apply_outline_cut_general(m, footprint(ring), options, extra);
    if (!result)
        std::cout << "returned " << result.error().path << ": " << result.error().reason << '\n';
    exact(m, original, "failure must restore every mesh field including signed zeros");
    require(!result, "invalid input must fail");
    check(result.error().path == path,
          "expected error path " + std::string(path) + ", got " + result.error().path);
    if (!reason.empty())
        check(result.error().reason == reason,
              "expected reason " + std::string(reason) + ", got " + result.error().reason);
    else
        require(!result.error().reason.empty(), "degenerate error must have a reason");
}
void invalid_vertex(Real bad, int axis) {
    auto ring = octagon();
    ring[2][axis] = bad;
    failure(ring, "outline", "outline polygon vertex outside outline.box");
}
void degenerate(const Ring& ring) {
    failure(ring, "outline.unsupported", "");
}
Ring l_ring() {
    return {{0, 0}, {.02, 0}, {.02, .0137}, {.0113, .0137}, {.0113, .02}, {0, .02}};
}
void invalid_options(int kind, bool nonconvex) {
    CutOptions o;
    switch (kind) {
    case 0:
        o.snap_phi = -1;
        break;
    case 1:
        o.snap_phi = std::numeric_limits<Real>::quiet_NaN();
        break;
    case 2:
        o.small_sub_vf = -1;
        break;
    case 3:
        o.small_sub_vf = 1;
        break;
    case 4:
        o.small_sub_vf = std::numeric_limits<Real>::quiet_NaN();
        break;
    default:
        o.quad_degree = 3;
    }
    failure(nonconvex ? l_ring() : octagon(), "outline", "cut/options: invalid thresholds", o);
}
void complete_removal(const Ring& ring) {
    // x <= -1 excludes every cell, independent of z origins or body IDs.
    failure(ring, "outline", "empty cut domain", {}, CutGeometry{{Plane{0, {1, 0, 0}, -1}}});
}
void bitwise_oracle() {
    Bits positive, negative;
    positive.add(0.);
    negative.add(-0.);
    require(positive.values == negative.values && positive.words != negative.words,
            "identity helper must distinguish signed zero in addition to value equality");
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
        run("bitwise_oracle", bitwise_oracle);
        run("convex_bitwise_octagon", convex_bitwise_octagon);
        run("convex_bitwise_collinear", convex_bitwise_collinear);
        run("convex_bitwise_demo_gate", convex_bitwise_demo_gate);
        run("empty_nullopt", empty_nullopt);
        run("duplicate_cleanup_convex", duplicate_cleanup_convex);
        for (int axis : {0, 1})
            for (const auto& [name, bad] :
                 {std::pair{"below_box", -.001},
                  std::pair{"above_box", .021},
                  std::pair{"nan", std::numeric_limits<Real>::quiet_NaN()},
                  std::pair{"pos_inf", std::numeric_limits<Real>::infinity()},
                  std::pair{"neg_inf", -std::numeric_limits<Real>::infinity()}})
                run(std::string("invalid_vertex_") + name + "_" + std::to_string(axis),
                    [=] { invalid_vertex(bad, axis); });
        run("degenerate_single", [] { degenerate({{.01, .01}}); });
        run("degenerate_two", [] { degenerate({{0, 0}, {.02, 0}}); });
        run("degenerate_collinear", [] { degenerate({{0, 0}, {.01, 0}, {.02, 0}}); });
        run("degenerate_duplicates", [] { degenerate({{.01, .01}, {.01, .01}, {.01, .01}}); });
        for (bool nonconvex : {false, true})
            for (int kind = 0; kind < 6; ++kind)
                run(std::string("invalid_options_") + (nonconvex ? "nonconvex_" : "convex_") +
                        std::to_string(kind),
                    [=] { invalid_options(kind, nonconvex); });
        run("rollback_empty_extra_removal", [] { complete_removal({}); });
        run("rollback_convex_extra_removal", [] { complete_removal(octagon()); });
        run("rollback_nonconvex_extra_removal", [] { complete_removal(l_ring()); });
        require(cases > 0, "unknown subcase filter");
        std::cout << cases << " cases, " << failures << " failed\n";
    });
    return harness == 0 && failures == 0 ? 0 : 1;
}
