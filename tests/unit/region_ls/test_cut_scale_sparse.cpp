#include "chipcae/mesh/cut/cut_stats.hpp"
#include "chipcae/mesh/cut/outline_cut.hpp"
#include "chipcae/mesh/cut/region_cut.hpp"
#include "support.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <optional>
#include <string_view>
#include <sys/resource.h>
#include <type_traits>
#include <utility>

namespace {
using chipcae::BodyId;
using chipcae::Index;
using chipcae::Real;
using namespace chipcae::mesh;
using namespace chipcae::mesh::cut;
using Ring = std::vector<std::array<Real, 2>>;

void check(bool condition, const std::string& message) {
    if (!condition)
        throw std::runtime_error(message);
}
template <class T>
T value(Expected<T> result) {
    if (!result)
        throw std::runtime_error(result.error().path + ": " + result.error().reason);
    return std::move(result.value());
}

// FNV-1a: lengths and integers are unsigned 64-bit little endian; Real retains
// its IEEE bits (including signed zero). No padding, native endian or addresses.
struct Hash {
    std::uint64_t digest = 14695981039346656037ULL;
    void byte(std::uint8_t v) {
        digest ^= v;
        digest *= 1099511628211ULL;
    }
    void word(std::uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            byte(static_cast<std::uint8_t>(v & 255));
            v >>= 8;
        }
    }
    template <class T>
        requires std::is_integral_v<T>
    void add(T v) {
        word(static_cast<std::uint64_t>(v));
    }
    void add(Real v) { word(std::bit_cast<std::uint64_t>(v)); }
    void add(const std::string& v) {
        word(v.size());
        for (unsigned char c : v)
            byte(c);
    }
    template <class T, std::size_t N>
    void add(const std::array<T, N>& v) {
        word(N);
        for (const auto& x : v)
            add(x);
    }
    template <class T>
    void add(const std::vector<T>& v) {
        word(v.size());
        for (const auto& x : v)
            add(x);
    }
    template <class K, class V>
    void add(const std::map<K, V>& v) {
        word(v.size());
        for (const auto& [key, val] : v) {
            add(key);
            add(val);
        }
    }
    template <class T>
    void add(const std::optional<T>& v) {
        add(v.has_value());
        if (v)
            add(*v);
    }
    void add(const AttributeRecord& r) {
        add(r.layer);
        add(r.region);
        add(r.material);
        add(r.body);
    }
    void add(const AttributeTable& t) {
        add(t.entries);
        add(t.references);
        add(t.bodies);
    }
    void add(const SnapReport& r) {
        add(r.axis);
        add(r.from);
        add(r.to);
        add(r.displacement);
    }
    void add(const HexMeshArrays::InterfaceReport& r) {
        add(r.name);
        add(r.side_a_bodies);
        add(r.side_b_bodies);
        add(r.aligned_faces);
        add(r.cut_faces);
        add(r.aligned_area);
        add(r.cut_area);
    }
    void add(const HexMeshArrays::GridReport& r) {
        add(r.levels);
        add(r.rule);
        add(r.N_L);
        add(r.auto_selected);
        add(r.padding);
        add(r.max_aspect_ratio);
        add(r.level_axes);
        add(r.column_ranks);
        add(r.warnings);
        add(r.partition_imbalance);
        add(r.void_cells);
    }
    void add(const BodyLevelSet& ls) {
        add(ls.body);
        add(ls.phi);
    }
    void add(const CutPolyhedra& p) {
        add(p.points);
        add(p.point_xi);
        add(p.point_parent_elem);
        add(p.face_conn);
        add(p.face_off);
        add(p.poly_faces);
        add(p.poly_off);
        add(p.parent_elem_gid);
        add(p.body);
        add(p.layer_id);
        add(p.material_id);
        add(p.cell_kind);
        add(p.volume);
        add(p.volfrac_ref);
    }
    void add(const CutLayerArrays& c) {
        add(c.polyhedra);
        add(c.level_sets);
        add(c.cut_elem);
        add(c.sub_off);
        add(c.sub_attribute);
        add(c.sub_volfrac);
        add(c.vq_off);
        add(c.bq_off);
        add(c.vq_xi);
        add(c.bq_xi);
        add(c.bq_n);
        add(c.vq_w);
        add(c.bq_w);
        add(c.bq_bdr_attr);
        add(c.gp_elem_gid);
        add(c.gp_face);
        add(c.gp_face_gid);
        add(c.gp_face_minus);
        add(c.gp_face_plus);
        add(c.gp_owner_rank);
        add(c.face_sets);
        add(c.eb_points);
        add(c.eb_xi);
        add(c.eb_normals);
        add(c.eb_parent_elem);
        add(c.sq_off);
        add(c.sq_side);
        add(c.sq_xi);
        add(c.sq_w);
        add(c.void_attribute);
        add(c.cut_elem_gid);
        add(c.void_elem);
        add(c.small_subs);
        add(c.exterior_area_vector);
    }
    // Named fields deliberately avoid binding the evolving HexMeshArrays aggregate.
    void add(const HexMeshArrays& m) {
        add(m.vertices);
        add(m.hex);
        add(m.attribute);
        add(m.bdr_quad);
        add(m.bdr_attribute);
        add(m.elem_gid);
        add(m.node_gid);
        add(m.partitioning);
        add(m.attr_table);
        add(m.face_sets);
        add(m.pending_cut);
    }
};
template <class T>
std::uint64_t hash(const T& v) {
    Hash h;
    h.add(v);
    return h.digest;
}
// Full-state checks are independent of the contracted golden
// digest. Unrelated mesh metadata must not invalidate pre-refactor goldens.
std::uint64_t full_mesh_hash(const HexMeshArrays& m) {
    Hash h;
    h.add(m);
    h.add(m.node_layer);
    h.add(m.copper_layer_of_layer);
    h.add(m.copper_phi);
    h.add(m.node_column);
    h.add(m.attr_frame);
    h.add(m.nodes);
    h.add(m.internal_interfaces);
    h.add(m.order);
    h.add(m.preparation_report);
    h.add(m.grid_report);
    h.add(m.attr_table.layers);
    h.add(m.attr_table.materials);
    h.add(m.attr_table.boundary_names);
    h.add(m.snapped);
    h.add(m.axes);
    return h.digest;
}

Ring octagon() {
    return {{.002, 0},
            {.018, 0},
            {.02, .002},
            {.02, .018},
            {.018, .02},
            {.002, .02},
            {0, .018},
            {0, .002}};
}
enum class Lid { Box, Convex, Notched };
StackModel stack(Lid lid = Lid::Box) {
    StackModel s;
    s.id = "synthetic_cut_scale_sparse";
    s.unit = "m";
    s.symmetry = "none";
    s.package_type = "FCBGA";
    s.outline = {{0, 0, .02, .02}, octagon()};
    Layer substrate;
    substrate.name = "substrate";
    substrate.kind = "Homogenized";
    substrate.material = "organic_eq";
    substrate.body = "substrate";
    substrate.thickness = .001;
    Layer bump;
    bump.name = "bump_eq";
    bump.kind = "Homogenized";
    bump.material = "sac305_ball_eq";
    bump.body = "bump";
    bump.thickness = .0001;
    Layer die;
    die.name = "die_layer";
    die.kind = "Explicit";
    die.material = lid == Lid::Box ? "underfill" : "";
    die.body = "underfill";
    die.thickness = .0005;
    Inclusion region{"die", "silicon", "die", {{.005, .005, .015, .015}, {}}};
    if (lid == Lid::Convex)
        region.footprint.polygon = {{.006, .005},
                                    {.014, .005},
                                    {.015, .006},
                                    {.015, .014},
                                    {.014, .015},
                                    {.006, .015},
                                    {.005, .014},
                                    {.005, .006}};
    if (lid == Lid::Notched)
        region.footprint.polygon = {{.006, .005},
                                    {.008, .005},
                                    {.008, .009},
                                    {.012, .009},
                                    {.012, .005},
                                    {.014, .005},
                                    {.015, .006},
                                    {.015, .014},
                                    {.014, .015},
                                    {.006, .015},
                                    {.005, .014},
                                    {.005, .006}};
    die.regions = {region};
    s.layers = {substrate, bump, die};
    for (auto& layer : s.layers) {
        layer.n_elem_min = 2;
        layer.grading = 1;
    }
    return s;
}
GridOptions grid_options(Local n) {
    GridOptions o;
    o.nx = o.ny = n;
    o.levels = 1;
    o.max_ratio = 10;
    o.ranks = 1;
    o.cell_budget = 10000000;
    o.snap_tol = 1e-14;
    return o;
}
CutOptions cut_options() {
    CutOptions o;
    o.snap_phi = 1e-14;
    o.small_sub_vf = 1e-8;
    o.quad_degree = 6;
    o.keep_void = false;
    o.export_polyhedra = false;
    return o;
}
HexMeshArrays grid(const StackModel& s, Local n) {
    auto background = s;
    background.outline.polygon.clear();
    return value(build_tensor_grid(background, grid_options(n)));
}
CutLayerArrays outline(HexMeshArrays& m, const StackModel& s) {
    const auto extra = value(region_cut_planes(s, m));
    auto result = value(apply_outline_cut(m, s.outline, cut_options(), extra));
    check(result.has_value(), "outline must produce cut arrays");
    check(result->polyhedra.has_value(), "apply_outline_cut must export polyhedra");
    check(!result->cut_elem.empty(), "slanted chamfers must partially cut cells");
    return std::move(*result);
}

struct Golden {
    std::uint64_t mesh, cut;
};
// Collect every fixture before failing so a pre-refactor run on another platform
// can report its complete baseline. Mismatches still fail the containing case.
[[nodiscard]] std::string
golden(const std::string& name, const HexMeshArrays& m, const CutLayerArrays& c, Golden g) {
    const Golden actual{hash(m), hash(c)};
    std::cout << "DIGEST " << name << " mesh=0x" << std::hex << actual.mesh << " cut=0x"
              << actual.cut << std::dec << '\n';
    std::string errors;
    if (actual.mesh != g.mesh)
        errors += name + ": mesh differs from the pre-refactor baseline; ";
    if (actual.cut != g.cut)
        errors += name + ": cut differs from the pre-refactor baseline; ";
    return errors;
}

// This reference remains independent of finalize's proposed compact face table.
void gp_reference(const HexMeshArrays& m, const CutLayerArrays& c) {
    ActivationMask active;
    for (std::size_t a = 1; a <= m.attr_table.entries.size(); ++a)
        if (static_cast<Local>(a) != c.void_attribute)
            active.push_back(static_cast<Local>(a));
    const auto reference = value(GhostPenaltyCache::prepare(m, c, {active}));
    const auto& f = reference.faces;
    check(!f.face_gid.empty(), "GP comparison must exercise shared cut faces");
    check(c.gp_elem_gid == f.elem_gid, "gp_elem_gid order/values differ from cache");
    check(c.gp_face == f.face, "gp_face differs from cache");
    check(c.gp_face_gid == f.face_gid, "gp_face_gid order/values differ from cache");
    check(c.gp_face_minus == f.face_minus, "gp_face_minus differs from cache");
    check(c.gp_face_plus == f.face_plus, "gp_face_plus differs from cache");
    check(c.gp_owner_rank == f.owner_rank, "gp_owner_rank differs from cache");
}

struct VolumeCheck {
    std::string name;
    CutStats actual;
    std::map<BodyId, Real> expected;
};
std::map<BodyId, Real> analytic_bodies(const StackModel& s, const HexMeshArrays& m) {
    // Independent SI geometry: the outer square loses four right triangles;
    // the lid loses four 1 mm chamfers and, for N, a 4 x 4 mm rectangular notch.
    constexpr Real area = .02 * .02 - 4 * .5 * .002 * .002;
    std::map<std::string, Real> expected{{"substrate", area * .001}, {"bump", area * .0001}};
    const auto& region = s.layers[2].regions[0];
    if (region.footprint.polygon.empty()) {
        expected["die"] = .01 * .01 * .0005;
        expected["underfill"] = (area - .01 * .01) * .0005;
    } else {
        Real lid_area = .01 * .01 - 4 * .5 * .001 * .001;
        if (region.footprint.polygon.size() == 12)
            lid_area -= .004 * .004;
        expected["die"] = lid_area * .0005;
    }
    std::map<BodyId, Real> result;
    for (const auto& [name, volume] : expected)
        result.emplace(m.attr_table.bodies.at(name), volume);
    return result;
}
void remember_volume(std::vector<VolumeCheck>& volumes,
                     const std::string& name,
                     const StackModel& s,
                     const HexMeshArrays& m,
                     const CutLayerArrays& c) {
    volumes.push_back({name, value(cut_stats(m, c)), analytic_bodies(s, m)});
}
void check_analytic_volumes(const std::vector<VolumeCheck>& volumes) {
    // Captured before production changes on ced6409 (same cut code as 784fe34),
    // GNU 14.2.0, Linux x86_64, Release. Keep n=64 accumulation bitwise fixed.
    const std::map<BodyId, std::uint64_t> scale_bodies{{1, 0x3e9a4e823b7b30f0ULL},
                                                       {2, 0x3e650b9b62c9466aULL},
                                                       {3, 0x3e83988594cc3858ULL},
                                                       {4, 0x3e6ad7f29abc9f05ULL}};
    constexpr std::uint64_t scale_total = 0x3ea50b9b62c8ff8eULL;
    std::string errors;
    for (const auto& v : volumes) {
        const bool scale = v.name == "scale_n64";
        const Real tolerance = scale ? 1e-11 : 1e-12;
        Real total = 0;
        check(v.actual.body_volume.size() == v.expected.size(), v.name + ": body count");
        for (const auto& [body, expected] : v.expected) {
            total += expected;
            const auto actual = v.actual.body_volume.at(body);
            if (scale && std::bit_cast<std::uint64_t>(actual) != scale_bodies.at(body))
                errors += v.name + ": body " + std::to_string(body) + " baseline bits; ";
            const Real error = std::abs(actual / expected - 1);
            std::cout << "BODY_VOLUME " << v.name << " body=" << body << " actual=" << actual
                      << " bits=0x" << std::hex << std::bit_cast<std::uint64_t>(actual) << std::dec
                      << " expected=" << expected << " relative_error=" << error << '\n';
            if (!(error <= tolerance))
                errors += v.name + ": body " + std::to_string(body) + " volume; ";
        }
        if (scale && std::bit_cast<std::uint64_t>(v.actual.solid_volume) != scale_total)
            errors += v.name + ": total baseline bits; ";
        const Real error = std::abs(v.actual.solid_volume / total - 1);
        std::cout << "VOLUME " << v.name << " bits=0x" << std::hex
                  << std::bit_cast<std::uint64_t>(v.actual.solid_volume) << std::dec
                  << " expected=" << total << " relative_error=" << error << '\n';
        if (!(error <= tolerance))
            errors += v.name + ": total volume; ";
        if (v.expected.size() == 4 &&
            !(std::abs(v.actual.solid_volume / 6.272e-7 - 1) <= tolerance))
            errors += v.name + ": DEMO-GATE volume 6.272e-7; ";
    }
    check(errors.empty(), errors);
}
void volume_analytic(const std::vector<VolumeCheck>& volumes) {
    check(volumes.size() == 9, "all acceptance fixtures must contribute analytic volumes");
    check_analytic_volumes(volumes);
}

Real rss_kib() {
    struct rusage usage{};
    check(getrusage(RUSAGE_SELF, &usage) == 0, "getrusage must succeed");
#if defined(__APPLE__)
    return static_cast<Real>(usage.ru_maxrss) / 1024;
#else
    return static_cast<Real>(usage.ru_maxrss);
#endif
}
struct Measurement {
    Real t_grid, t_cut, r0, r1, r2;
};
Measurement scale_guard(std::vector<VolumeCheck>& volumes) {
    const auto s = stack();
    auto background = s;
    background.outline.polygon.clear();
    const auto options = grid_options(64);
    const auto cutting = cut_options();
    // Five same-process samples reduce scheduler noise on both platforms. Use
    // each duration's median, retaining the original time and RSS thresholds.
    // ru_maxrss only grows: measure memory on the first build/cut, before later
    // samples can reuse allocations or inherit an earlier sample's high-water mark.
    std::array<Real, 5> grid_times{}, cut_times{};
    Measurement result{};
    std::size_t cells = 0, cut_cells = 0;
    for (std::size_t sample = 0; sample < grid_times.size(); ++sample) {
        const Real r0 = rss_kib();
        const auto start = std::chrono::steady_clock::now();
        auto built_mesh = build_tensor_grid(background, options);
        const auto built = std::chrono::steady_clock::now();
        auto m = value(std::move(built_mesh));
        const Real r1 = rss_kib();
        const auto built_cells = m.attribute.size();
        const auto cut_start = std::chrono::steady_clock::now();
        const auto extra = value(region_cut_planes(s, m));
        auto c = value(apply_outline_cut(m, s.outline, cutting, extra));
        const auto cut_end = std::chrono::steady_clock::now();
        const Real r2 = rss_kib();
        check(c.has_value(), "scale_guard must exercise outline cutting");
        grid_times[sample] = std::chrono::duration<Real>(built - start).count();
        cut_times[sample] = std::chrono::duration<Real>(cut_end - cut_start).count();
        std::cout << "MEASURE_SAMPLE cut-scale-sparse: {\"sample\":" << sample + 1
                  << ",\"t_grid\":" << grid_times[sample] << ",\"t_cut\":" << cut_times[sample]
                  << "}" << std::endl;
        if (sample == 0) {
            result.r0 = r0;
            result.r1 = r1;
            result.r2 = r2;
            cells = built_cells;
            cut_cells = c->cut_elem.size();
            remember_volume(volumes, "scale_n64", s, m, *c);
        }
    }
    std::ranges::sort(grid_times);
    std::ranges::sort(cut_times);
    result.t_grid = grid_times[grid_times.size() / 2];
    result.t_cut = cut_times[cut_times.size() / 2];
    std::cout << "MEASURE cut-scale-sparse: {\"n\":64,\"cells\":" << cells
              << ",\"cut_cells\":" << cut_cells << ",\"t_grid\":" << result.t_grid
              << ",\"t_cut\":" << result.t_cut << ",\"r0_kib\":" << result.r0
              << ",\"r1_kib\":" << result.r1 << ",\"r2_kib\":" << result.r2 << "}" << std::endl;
    return result;
}

void bitwise_outline(std::vector<VolumeCheck>& volumes) {
    constexpr std::array<Local, 4> resolutions{4, 8, 16, 32};
    // Captured from untouched pre-refactor cut code (ced6409, identical to
    // 784fe34), GNU 14.2.0, Linux x86_64, Release. Mesh digests were recaptured
    // with only the brief's named fields; cut digests remain unchanged.
    constexpr std::array<Golden, 4> expected{{
        {0xa704cc1efb6e7346ULL, 0x710e70433c2a85eeULL},
        {0xb7066576e2afffe0ULL, 0xd854e053fc2c3cefULL},
        {0x95d1f16842b0297dULL, 0xa1261a1ef02de996ULL},
        {0x4f3fe256a94ed292ULL, 0x2eb75ee8bb73c3c4ULL},
    }};
    std::string errors;
    for (std::size_t i = 0; i < resolutions.size(); ++i) {
        const auto n = resolutions[i];
        const auto s = stack();
        auto m = grid(s, n);
        const auto c = outline(m, s);
        const auto name = "outline_n" + std::to_string(n);
        errors += golden(name, m, c, expected[i]);
        gp_reference(m, c);
        remember_volume(volumes, name, s, m, c);
    }
    check(errors.empty(), errors);
}
void bitwise_snap() {
    const auto s = stack();
    auto m = grid(s, 10);
    for (std::size_t d = 0; d < 2; ++d)
        check(std::ranges::any_of(m.axes[d],
                                  [](Real x) {
                                      return std::bit_cast<std::uint64_t>(x) ==
                                             std::bit_cast<std::uint64_t>(.002);
                                  }),
              "snap fixture must contain the exact .002 grid line in x and y");
    const auto c = outline(m, s);
    const auto errors = golden("outline_n10", m, c, {0x0d4fac692083ee07ULL, 0x953382a220d2e108ULL});
    gp_reference(m, c);
    std::vector<VolumeCheck> volumes;
    remember_volume(volumes, "outline_n10", s, m, c);
    check_analytic_volumes(volumes);
    check(errors.empty(), errors);
}

// Nondefault metadata makes a whole-object replacement or missing compaction
// observable even when the contracted mesh digest would stay unchanged.
void seed_mesh_metadata(HexMeshArrays& m) {
    for (std::size_t v = 0; v < m.node_gid.size(); ++v) {
        m.node_layer[v] = static_cast<Local>(v % 7);
        m.node_column[v] = static_cast<Index>(3 * v + 11);
    }
    m.copper_layer_of_layer = {2, -1, 0};
    m.copper_phi = {.25, -.5, .75};
    m.attr_frame.assign(9 * m.attr_table.entries.size(), .25);
    m.nodes = {.125, -.25, .5};
    m.internal_interfaces = true;
    m.order = 2;
    m.preparation_report.push_back({"metadata", {"substrate"}, {"die"}, 2, 3, .25, .5});
    m.grid_report.levels = 2;
    m.grid_report.rule = 3;
    m.grid_report.N_L = 19;
    m.grid_report.auto_selected = true;
    m.grid_report.padding = {2, 3};
    m.grid_report.max_aspect_ratio = {1.25, 2.5};
    m.grid_report.level_axes = {{{{0, .01, .02}, {0, .02}}}};
    m.grid_report.column_ranks = {{0, 1}, {1, 0}};
    m.grid_report.warnings = {"synthetic grid metadata"};
    m.grid_report.partition_imbalance = .125;
    m.grid_report.void_cells = 17;
    m.pending_cut = {
        0, static_cast<Local>(m.attribute.size() / 2), static_cast<Local>(m.attribute.size() - 1)};
    m.snapped.push_back({'x', .001, .002, .001});
}
void bitwise_lid(std::vector<VolumeCheck>& volumes, Lid lid) {
    const auto s = stack(lid);
    check(s.layers[2].material.empty(), "polygon lid must have no background material");
    auto m = grid(s, 8);
    const auto g = value(region_cut_planes(s, m));
    const auto body = m.attr_table.bodies.at("die");
    if (lid == Lid::Convex) {
        check(g.unions.empty() && g.planes.size() == 8, "convex lid must add eight planes");
        for (const auto& p : g.planes)
            check(p.body == body && p.body != 0, "region planes must be limited to the lid body");
    } else {
        check(g.unions.size() == 1 && g.unions[0].body == body && g.unions[0].pieces.size() > 1,
              "notched lid must exercise a multi-piece PieceUnion");
    }
    const auto c = outline(m, s);
    const std::string name = lid == Lid::Convex ? "region_lid" : "union";
    const auto errors =
        golden(name,
               m,
               c,
               lid == Lid::Convex ? Golden{0x2edefd971f79fe98ULL, 0x28db459fa37df24fULL}
                                  : Golden{0xb6bf7b0323290597ULL, 0x976fff2710cb69baULL});
    gp_reference(m, c);
    remember_volume(volumes, name, s, m, c);
    check(errors.empty(), errors);
}
CutGeometry outer_planes() {
    CutGeometry g;
    const auto ring = octagon();
    for (std::size_t i = 0; i < ring.size(); ++i) {
        const auto a = ring[i], b = ring[(i + 1) % ring.size()];
        const Real length = std::hypot(b[1] - a[1], a[0] - b[0]);
        const Real nx = (b[1] - a[1]) / length, ny = (a[0] - b[0]) / length;
        g.planes.push_back({0, {nx, ny, 0}, nx * a[0] + ny * a[1]});
    }
    return g;
}
void keep_void_both(std::vector<VolumeCheck>& volumes) {
    const auto s = stack();
    const auto initial = grid(s, 8);
    const auto g = outer_planes();
    const auto plan = value(build_cut_layer(initial, g, cut_options()));
    check(!plan.void_elem.empty(), "fixture must contain entirely exterior cells");
    std::string errors;
    for (const bool keep : {false, true}) {
        auto m = initial;
        auto options = cut_options();
        options.keep_void = keep;
        const auto c = value(build_cut_layer(m, g, options));
        check(!c.polyhedra.has_value(), "direct build with export=false must omit polyhedra");
        if (keep) {
            check(c.void_elem.size() == plan.void_elem.size() && c.void_attribute > 0,
                  "keep_void must retain all exterior cells");
            for (const auto e : c.void_elem)
                check(m.attribute[e] == c.void_attribute, "retained void must use void_attribute");
        } else {
            check(c.void_elem.empty(), "removed void list must be empty");
            for (const auto e : plan.void_elem)
                check(std::ranges::find(m.elem_gid, initial.elem_gid[e]) == m.elem_gid.end(),
                      "removed void GID must disappear");
        }
        const std::string name = keep ? "void_keep" : "void_remove";
        errors += golden(name,
                         m,
                         c,
                         keep ? Golden{0x0b1422792db4f4baULL, 0x883bf8fb0e99db04ULL}
                              : Golden{0xb7066576e2afffe0ULL, 0x55523e514716771dULL});
        gp_reference(m, c);
        remember_volume(volumes, name, s, m, c);
    }
    check(errors.empty(), errors);
}
void successful_mesh_state() {
    // Captured from unchanged ced6409 cut sources, GNU 14.2.0, Linux x86_64,
    // Release. Keep these separate from the brief's named-field goldens.
    constexpr std::array<std::uint64_t, 3> outline_expected{
        0xb0bfd3e93428442bULL, 0x23583fe378c251c9ULL, 0xfe4e8beccf17b066ULL};
    constexpr std::array<std::uint64_t, 2> direct_expected{0xb0bfd3e93428442bULL,
                                                           0x0fced36ddccee91aULL};
    std::string errors;
    const auto verify =
        [&](const std::string& name, const HexMeshArrays& m, std::uint64_t expected) {
            const auto actual = full_mesh_hash(m);
            std::cout << "FULL_MESH_DIGEST " << name << " actual=0x" << std::hex << actual
                      << " expected=0x" << expected << std::dec << '\n';
            if (actual != expected)
                errors += name + ": successful cut must match every pre-refactor mesh field; ";
        };
    std::size_t i = 0;
    for (const auto lid : {Lid::Box, Lid::Convex, Lid::Notched}) {
        const auto s = stack(lid);
        auto m = grid(s, 8);
        seed_mesh_metadata(m);
        outline(m, s);
        verify("outline_" + std::to_string(i), m, outline_expected[i]);
        ++i;
    }
    for (const bool keep : {false, true}) {
        auto m = grid(stack(), 8);
        seed_mesh_metadata(m);
        auto options = cut_options();
        options.keep_void = keep;
        value(build_cut_layer(m, outer_planes(), options));
        verify(keep ? "void_keep" : "void_remove", m, direct_expected[keep]);
    }
    check(errors.empty(), errors);
}
void attribute_frame_identity() {
    // Finalization must allocate every identity frame, including a newly added
    // void attribute, regardless of whether the input frames are empty or stale.
    for (const bool keep : {false, true})
        for (const bool empty : {false, true}) {
            auto m = grid(stack(), 4);
            if (empty)
                m.attr_frame.clear();
            else
                m.attr_frame.assign(9 * m.attr_table.entries.size(), .25);
            auto options = cut_options();
            options.keep_void = keep;
            const auto c = value(build_cut_layer(m, outer_planes(), options));
            check(!c.cut_elem.empty(), "frame fixture must exercise cut finalization");
            check(!keep || c.void_attribute > 0, "frame fixture must add a void attribute");
            check(m.attr_frame.size() == 9 * m.attr_table.entries.size(),
                  "finalization must size frames for every output attribute");
            for (std::size_t a = 0; a < m.attr_table.entries.size(); ++a)
                for (std::size_t row = 0; row < 3; ++row)
                    for (std::size_t col = 0; col < 3; ++col) {
                        const Real expected = row == col ? 1 : 0;
                        check(std::bit_cast<std::uint64_t>(m.attr_frame[9 * a + 3 * row + col]) ==
                                  std::bit_cast<std::uint64_t>(expected),
                              "finalization must initialize the complete identity frame");
                    }
        }
}
void error_leaves_mesh() {
    const auto s = stack();
    auto m = grid(s, 8);
    const auto before = full_mesh_hash(m);
    auto outside = s.outline;
    outside.polygon[0][0] = -.001;
    auto bad = apply_outline_cut(m, outside, cut_options());
    check(!bad && bad.error().path == "outline" &&
              bad.error().reason == "outline polygon vertex outside outline.box",
          "outside vertex must return the exact outline path/reason");
    check(full_mesh_hash(m) == before, "outside-vertex error must preserve every mesh field");
    auto empty =
        apply_outline_cut(m, s.outline, cut_options(), CutGeometry{{Plane{0, {1, 0, 0}, -1}}});
    check(!empty && empty.error().path == "outline" && empty.error().reason == "empty cut domain",
          "complete removal must return the exact outline path/reason");
    check(full_mesh_hash(m) == before, "empty-domain error must preserve every mesh field");
    // The mutable public builder, unlike outline, deliberately accepts empty results.
    const auto c = value(build_cut_layer(m, CutGeometry{{Plane{0, {1, 0, 0}, -1}}}, cut_options()));
    check(m.elem_gid.empty() && c.void_elem.empty(), "mutable build must accept empty results");
}
void digest_scope() {
    auto m = grid(stack(), 4);
    const auto before = hash(m);
    const auto full_before = full_mesh_hash(m);
    m.node_layer.push_back(17);
    m.copper_layer_of_layer.push_back(18);
    m.copper_phi.push_back(.25);
    m.node_column.push_back(19);
    m.attr_frame.push_back(.5);
    m.nodes.push_back(.75);
    m.internal_interfaces = !m.internal_interfaces;
    ++m.order;
    HexMeshArrays::InterfaceReport report;
    report.name = "unrelated interface";
    m.preparation_report.push_back(report);
    ++m.grid_report.void_cells;
    m.snapped.push_back({'x', 0, 1, 1});
    m.axes[0].push_back(2);
    m.attr_table.layers.emplace("unrelated layer", 20);
    m.attr_table.materials.emplace("unrelated material", 21);
    m.attr_table.boundary_names.emplace(22, "unrelated boundary");
    check(hash(m) == before, "mesh baseline digest must include only the brief's named fields");
    check(full_mesh_hash(m) != full_before, "rollback digest must still cover unrelated fields");
}
void sequential_snap() {
    HexMeshArrays initial;
    initial.vertices = {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 0, 0, 1, 1, 0, 1, 1, 1, 1, 0, 1, 1};
    initial.hex = {0, 1, 2, 3, 4, 5, 6, 7};
    initial.node_gid = {0, 1, 2, 3, 4, 5, 6, 7};
    initial.node_layer.resize(8);
    initial.node_column.resize(8);
    initial.attribute = {1};
    initial.elem_gid = {7};
    initial.partitioning = {0};
    initial.attr_table.entries.push_back({"solid", "", 1, 1});
    initial.attr_table.bodies.emplace("solid", 1);
    const auto before = full_mesh_hash(initial);
    auto options = cut_options();
    options.snap_phi = .001;
    const Plane first{1, {1, 0, 0}, .99925};
    const Plane later{1, {1, 1, 0}, 1.99825};
    // The first plane snaps phi at x=1 to zero without moving those vertices.
    // The later plane must still cut a triangular prism of area .00175^2/2.
    // Projecting x=1 onto the first plane would incorrectly leave the cell whole.
    for (int mode = 0; mode < 3; ++mode) {
        CutGeometry g;
        if (mode == 0) {
            g.planes = {first, later};
        } else {
            PieceUnion region;
            region.body = 1;
            region.pieces = {{first, {1, {-1, 0, 0}, 0}, {1, {0, -1, 0}, 0}, {1, {0, 1, 0}, 1}}};
            region.ring = {{0, 0}, {.99925, 0}, {.99925, 1}, {0, 1}};
            if (mode == 2) {
                region.pieces[0].push_back(later);
                region.ring = {{0, 0}, {.99925, 0}, {.99925, .999}, {.99825, 1}, {0, 1}};
            } else {
                g.planes = {later};
            }
            region.internal = {std::vector<bool>(region.pieces[0].size(), false)};
            g.unions.push_back(std::move(region));
        }
        const auto c = value(build_cut_layer(std::as_const(initial), g, options));
        check(c.cut_elem == std::vector<Local>{0} && c.sub_volfrac.size() == 1,
              "later plane must cut after an earlier plane snaps only phi");
        check(std::abs(c.sub_volfrac[0] - (1 - .00175 * .00175 / 2)) <= 1e-12,
              "sequential snap volume must match the analytic triangular prism within 1e-12");
        check(full_mesh_hash(initial) == before, "const clipping must preserve the entire mesh");
    }
}
void gp_nonmanifold() {
    HexMeshArrays initial;
    initial.vertices = {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 0, 0, 1, 1, 0, 1, 1, 1, 1, 0, 1, 1};
    initial.node_gid = {0, 1, 2, 3, 4, 5, 6, 7};
    initial.node_layer.resize(8);
    initial.node_column.resize(8);
    // Three coincident cells give six nonmanifold face groups. The cache
    // retains the first occurrence and pairs both later occurrences with it.
    for (int i = 0; i < 3; ++i)
        initial.hex.insert(initial.hex.end(), {0, 1, 2, 3, 4, 5, 6, 7});
    initial.attribute = {1, 1, 1};
    initial.elem_gid = {30, 10, 20};
    initial.partitioning = {2, 1, 0};
    initial.attr_table.entries.push_back({"solid", "", 1, 1});
    initial.attr_table.bodies.emplace("solid", 1);
    for (const bool reverse : {false, true}) {
        auto m = initial;
        if (reverse)
            std::reverse(m.elem_gid.begin(), m.elem_gid.end());
        const auto c =
            value(build_cut_layer(m, CutGeometry{{Plane{0, {1, 1, 0}, 1.5}}}, cut_options()));
        check(c.cut_elem.size() == 3, "nonmanifold fixture must cut all three cells");
        gp_reference(m, c);
        check(c.gp_face_gid.size() == 12, "three shared cells must yield two pairs per face");
        for (std::size_t i = 0; i < c.gp_face_gid.size(); ++i) {
            const auto encounter = i < 6 ? m.elem_gid[1] : m.elem_gid[2];
            check(c.gp_elem_gid[0][i] == std::min(m.elem_gid[0], encounter) &&
                      c.gp_elem_gid[1][i] == std::max(m.elem_gid[0], encounter),
                  "nonmanifold GP must pair each later cell with the first cell");
            check(c.gp_face_minus[i] == static_cast<Local>(i % 6) &&
                      c.gp_face_plus[i] == static_cast<Local>(i % 6),
                  "nonmanifold GP must retain MFEM encounter order");
        }
    }
}
void gp_reordered_gids() {
    const auto s = stack();
    auto m = grid(s, 4);
    std::reverse(m.elem_gid.begin(), m.elem_gid.end());
    for (std::size_t e = 0; e < m.partitioning.size(); ++e)
        m.partitioning[e] = static_cast<Local>(e % 3);
    const auto c = outline(m, s);
    gp_reference(m, c);
    check(std::ranges::any_of(c.gp_owner_rank, [](Local rank) { return rank != 0; }),
          "GP owner check must exercise nonzero partitions");
    for (std::size_t i = 0; i < c.gp_face_gid.size(); ++i) {
        check(c.gp_elem_gid[0][i] < c.gp_elem_gid[1][i], "GP minus must use the smaller GID");
        check(c.gp_face_gid[i] == 6 * c.gp_elem_gid[0][i] + c.gp_face_minus[i],
              "GP identity must use the MFEM minus-face number");
    }
}
} // namespace

int main() {
    return sp5::run([] {
        std::cout << std::setprecision(17);
        std::vector<VolumeCheck> volumes;
        int failures = 0;
        const auto run = [&](std::string_view name, auto test) {
            try {
                test();
                std::cout << "cut-scale-sparse: " << name << " ok" << std::endl;
            } catch (const std::exception& e) {
                ++failures;
                std::cerr << "cut-scale-sparse: " << name << " FAIL: " << e.what() << std::endl;
            }
        };
        std::optional<Measurement> measured;
        // ru_maxrss is monotonic: no other fixture may precede this measurement.
        run("scale_guard measurement", [&] { measured = scale_guard(volumes); });
        run("bitwise_outline", [&] { bitwise_outline(volumes); });
        run("bitwise_snap", bitwise_snap);
        run("bitwise_region_lid", [&] { bitwise_lid(volumes, Lid::Convex); });
        run("bitwise_union", [&] { bitwise_lid(volumes, Lid::Notched); });
        run("keep_void_both", [&] { keep_void_both(volumes); });
        run("error_leaves_mesh", error_leaves_mesh);
        run("digest_scope", digest_scope);
        run("successful_mesh_state", successful_mesh_state);
        run("attribute_frame_identity", attribute_frame_identity);
        run("sequential_snap", sequential_snap);
        run("volume_analytic", [&] { volume_analytic(volumes); });
        run("gp_nonmanifold", gp_nonmanifold);
        run("gp_reordered_gids", gp_reordered_gids);
        // Defer performance assertions until all correctness results are collected.
        run("scale_guard", [&] {
            check(measured.has_value(), "scale measurement unavailable");
#ifdef NDEBUG
            const auto& m = *measured;
            const bool time_ok = m.t_cut <= m.t_grid;
            const bool memory_ok = m.r2 - m.r1 <= m.r1 - m.r0;
            if (!time_ok)
                std::cerr << "scale_guard: t_cut=" << m.t_cut << " > t_grid=" << m.t_grid << '\n';
            if (!memory_ok)
                std::cerr << "scale_guard: cut RSS increment=" << m.r2 - m.r1
                          << " > grid RSS increment=" << m.r1 - m.r0 << " KiB\n";
            check(time_ok && memory_ok, "Release requires t_cut<=t_grid and r2-r1<=r1-r0");
#endif
        });
        check(failures == 0, std::to_string(failures) + " acceptance case(s) failed");
    });
}
