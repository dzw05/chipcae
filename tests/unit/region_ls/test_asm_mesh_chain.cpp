#include "chipcae/geometry/assembly.hpp"
#include "chipcae/mesh/cut/cut_stats.hpp"
#include "chipcae/mesh/cut/outline_cut.hpp"
#include "chipcae/mesh/cut/region_cut.hpp"
#include "support.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
using chipcae::BodyId;
using chipcae::Index;
using chipcae::Real;
using namespace chipcae::geometry;
using namespace chipcae::mesh;
using namespace chipcae::mesh::cut;
using sp5::require;

void check(bool ok, const std::string& message) {
    require(ok, ("asm mesh chain: " + message).c_str());
}

template <class T>
T value(chipcae::mesh::Expected<T> result, const std::string& operation) {
    if (!result)
        check(false, operation + " [" + result.error().path + "] " + result.error().reason);
    return std::move(result.value());
}

template <class T>
T value(chipcae::geometry::Expected<T> result, const std::string& operation) {
    if (!result)
        check(false, operation + ": " + result.error().message);
    return std::move(result.value());
}

Real threshold(const std::string& key) {
    std::ifstream in(std::string(CHIPCAE_SOURCE_DIR) + "/mesh/background/thresholds/k1.json");
    check(bool(in), "threshold file opens");
    const std::string text((std::istreambuf_iterator<char>(in)), {});
    const auto pos = text.find('"' + key + '"');
    check(pos != std::string::npos, "threshold key exists: " + key);
    const auto colon = text.find(':', pos);
    check(colon != std::string::npos, "threshold value exists: " + key);
    return std::stod(text.substr(colon + 1));
}

struct Options {
    GridOptions grid;
    CutOptions cut;
    Real volume_relative;

    Options() : volume_relative(threshold("cut_volume_relative")) {
        grid.cell_budget = static_cast<Index>(threshold("cell_budget"));
        grid.snap_tol = threshold("snap_tol");
        grid.max_ratio = threshold("max_ratio");
        grid.levels = 1;
        grid.ranks = 1;
        cut.quad_degree = 6;
        cut.keep_void = false;
        cut.export_polyhedra = false;
        cut.snap_phi = threshold("snap_phi");
        cut.small_sub_vf = threshold("small_sub_vf");
    }
};

std::vector<std::array<Real, 2>> lid_polygon() {
    return {{.002, 0},
            {.054, 0},
            {.056, .002},
            {.056, .042},
            {.054, .044},
            {.002, .044},
            {0, .042},
            {0, .002}};
}

void near(Real actual, Real expected, const std::string& field) {
    check(std::isfinite(actual) && std::abs(actual - expected) <= 1e-15, field);
}

template <std::size_t N>
void near(const std::array<Real, N>& actual,
          const std::array<Real, N>& expected,
          const std::string& field) {
    for (std::size_t i = 0; i < N; ++i)
        near(actual[i], expected[i], field + "[" + std::to_string(i) + "]");
}

// Changing strip decomposition, body assignment, or polygon normalization must fail here,
// before the assembly output is passed unmodified to the grid builder.
void assembly(const MergeResult& merged) {
    check(merged.defects.empty(), "merge has no defects");
    const auto& layers = merged.stack.layers;
    check(layers.size() == 3, "exactly three ordered layers");
    const std::array<std::string, 3> names{"substrate", "assembly#1", "assembly#2"};
    const std::array<Real, 3> thicknesses{.001, .0005, .0005};
    for (std::size_t i = 0; i < layers.size(); ++i) {
        check(layers[i].name == names[i], "ordered layer name " + std::to_string(i));
        near(layers[i].thickness, thicknesses[i], "layer thickness " + names[i]);
    }
    const auto& substrate = layers[0];
    check(substrate.kind == "Explicit" && substrate.material == "organic_eq" &&
              substrate.body == "substrate" && substrate.regions.empty(),
          "substrate metadata retained");
    near(substrate.grading, 1, "substrate grading retained");
    const auto& strips = layers[1].regions;
    check(strips.size() == 4, "stiffener splits into four ordered strips");
    const std::array<std::array<Real, 4>, 4> boxes{{{.008, .002, .013, .042},
                                                    {.013, .002, .043, .007},
                                                    {.013, .037, .043, .042},
                                                    {.043, .002, .048, .042}}};
    for (std::size_t i = 0; i < strips.size(); ++i) {
        const auto& r = strips[i];
        check(r.name == "stiffener#0#" + std::to_string(i) && r.material == "copper" &&
                  r.body == "stiffener" && r.footprint.polygon.empty(),
              "ordered stiffener strip metadata " + std::to_string(i));
        near(r.footprint.box, boxes[i], r.name + " box");
    }
    check(layers[2].regions.size() == 1, "lid has one region");
    const auto& lid = layers[2].regions.front();
    check(lid.name == "lid#0#0" && lid.material == "copper" && lid.body == "lid", "lid metadata");
    near(lid.footprint.box, std::array<Real, 4>{0, 0, .056, .044}, "lid bounding box");
    auto expected = lid_polygon();
    Real twice_area = 0;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const auto& a = expected[i];
        const auto& b = expected[(i + 1) % expected.size()];
        twice_area += a[0] * b[1] - b[0] * a[1];
    }
    if (twice_area < 0)
        std::reverse(expected.begin(), expected.end());
    std::rotate(
        expected.begin(), std::min_element(expected.begin(), expected.end()), expected.end());
    check(lid.footprint.polygon.size() == 8, "lid retains eight polygon points");
    for (std::size_t i = 0; i < expected.size(); ++i)
        near(lid.footprint.polygon[i], expected[i], "normalized lid vertex " + std::to_string(i));
}

StackModel merged_stack(Local n_elem_min) {
    StackModel base;
    base.id = "board";
    base.outline.box = {0, 0, .056, .044};
    Layer substrate;
    substrate.name = "substrate";
    substrate.kind = "Explicit";
    substrate.material = "organic_eq";
    substrate.body = "substrate";
    substrate.thickness = .001;
    substrate.n_elem_min = 1;
    substrate.grading = 1;
    base.layers = {substrate};
    const SlabSection ring{Ring{{{.008, .002}, {.048, .002}, {.048, .042}, {.008, .042}}},
                           {Ring{{{.013, .007}, {.043, .007}, {.043, .037}, {.013, .037}}}}};
    const std::vector<PlacedPart> parts{
        {"stiffener", "copper", {{.001, .0015, {ring}}}},
        {"lid", "copper", {{.0015, .002, {{Ring{lid_polygon()}, {}}}}}}};
    auto merged = value(merge_parts(base, parts, {1e-5, 1e-7, 1e-8}), "merge_parts");
    assembly(merged);
    for (auto& layer : merged.stack.layers)
        layer.n_elem_min = n_elem_min;
    return std::move(merged.stack);
}

void count(std::size_t actual, std::size_t expected, const std::string& field) {
    check(actual == expected,
          field + ": expected " + std::to_string(expected) + ", got " + std::to_string(actual));
}

struct Scale {
    Local n;
    std::size_t x, y, elements, pending;
};
constexpr std::array<Scale, 4> scales{{{1, 14, 25, 808, 350},
                                       {2, 26, 48, 2867, 1248},
                                       {4, 48, 92, 10123, 4410},
                                       {8, 92, 180, 37915, 16526}}};

struct Run {
    HexMeshArrays before, after;
    CutLayerArrays cut;
    CutStats stats;
};

Real body_volume(const Run& run, const std::string& name) {
    const auto body = run.after.attr_table.bodies.find(name);
    check(body != run.after.attr_table.bodies.end(), "registered body " + name);
    const auto volume = run.stats.body_volume.find(body->second);
    check(volume != run.stats.body_volume.end(), "volume entry for " + name);
    return volume->second;
}

void volume(Real actual, Real expected, Real tolerance, const std::string& name) {
    check(std::isfinite(actual), name + " volume is finite");
    const auto error = std::abs(actual - expected) / std::abs(expected);
    check(error <= tolerance, name + " volume relative error " + std::to_string(error));
}

Run pipeline(const Options& options, const Scale& scale, Local n_elem_min = 1) {
    std::clog << "asm mesh chain: nx=ny=" << scale.n << " n_elem_min=" << n_elem_min << '\n';
    const auto stack = merged_stack(n_elem_min);
    auto grid_options = options.grid;
    grid_options.nx = grid_options.ny = scale.n;
    Run run;
    run.before = value(build_tensor_grid(stack, grid_options), "build_tensor_grid");
    for (const auto& axis : run.before.axes)
        check(axis.size() >= 2, "axis has intervals");
    count(run.before.axes[0].size() - 1, scale.x, "X intervals");
    count(run.before.axes[1].size() - 1, scale.y, "Y intervals");
    count(run.before.axes[2].size() - 1, 3 * static_cast<std::size_t>(n_elem_min), "nz");
    count(run.before.attribute.size(), scale.elements * n_elem_min, "pre-cut elements");
    count(run.before.pending_cut.size(), scale.pending * n_elem_min, "pre-cut pending");
    const auto planes = value(region_cut_planes(stack, run.before), "region_cut_planes");
    count(planes.planes.size(), 8, "extracted planes");
    const auto lid = run.before.attr_table.bodies.find("lid");
    check(lid != run.before.attr_table.bodies.end(), "lid registered before cutting");
    std::set<BodyId> plane_bodies;
    for (const auto& plane : planes.planes) {
        check(plane.body == lid->second, "all eight planes scoped to lid");
        plane_bodies.insert(plane.body);
    }
    // Check coverage before apply_outline_cut compacts and remaps the mesh.
    for (const auto e : run.before.pending_cut) {
        check(e >= 0 && static_cast<std::size_t>(e) < run.before.attribute.size(),
              "pending index is valid");
        const auto attr = run.before.attribute[static_cast<std::size_t>(e)];
        check(attr > 0 && static_cast<std::size_t>(attr) <= run.before.attr_table.entries.size(),
              "pending attribute is valid");
        const auto body = run.before.attr_table.entries[static_cast<std::size_t>(attr - 1)].body;
        check(body == lid->second && plane_bodies.contains(body), "pending body has lid planes");
    }
    run.after = run.before;
    auto cut = value(apply_outline_cut(run.after, stack.outline, options.cut, planes),
                     "apply_outline_cut");
    check(cut.has_value(), "apply_outline_cut returns cut arrays");
    run.cut = std::move(*cut);
    run.stats = value(cut_stats(run.after, run.cut), "cut_stats with compacted mesh");
    count(run.stats.body_volume.size(), 3, "exactly three body volume entries");
    // Analytic SI volumes: board area * thickness; annulus area * thickness;
    // board area minus four triangular chamfers, times lid thickness.
    const std::array<std::string, 3> names{"substrate", "stiffener", "lid"};
    const std::array<Real, 3> expected{2.464e-6, 3.5e-7, 1.228e-6};
    for (std::size_t i = 0; i < names.size(); ++i)
        volume(body_volume(run, names[i]), expected[i], options.volume_relative, names[i]);
    volume(run.stats.solid_volume, 4.042e-6, options.volume_relative, "solid");
    volume(body_volume(run, "lid"), expected[2], 1e-12, "lid strict");
    volume(run.stats.solid_volume, 4.042e-6, 1e-12, "solid strict");
    count(run.stats.small_subs_recorded, run.cut.small_subs.size(), "small-subcell statistics");
    count(run.cut.small_subs.size(), 0, "no small subcells");
    for (const auto* name : {"assembly#1", "assembly#2"}) {
        const auto body = run.after.attr_table.bodies.find(name);
        check(body != run.after.attr_table.bodies.end(), std::string("empty layer body ") + name);
        check(run.stats.body_volume.find(body->second) == run.stats.body_volume.end(),
              std::string("no volume for empty layer body ") + name);
    }
    return run;
}

void budget(const Options& options,
            Local n_elem_min,
            Local nx,
            Local ny,
            std::size_t expected_x,
            std::size_t expected_y) {
    const auto stack = merged_stack(n_elem_min);
    auto grid_options = options.grid;
    grid_options.nx = nx;
    grid_options.ny = ny;
    const auto grid = build_tensor_grid(stack, grid_options);
    check(!grid, "over-budget grid must fail");
    check(grid.error().path == "HIERARCHY_INVALID",
          "budget error path: " + grid.error().path + ": " + grid.error().reason);
    const auto candidates =
        value(hierarchy_candidates(stack, grid_options), "hierarchy_candidates");
    check(!candidates.empty(), "budget candidates are nonempty");
    const auto& first = candidates.front();
    check(first.L == 0 && !first.feasible, "first candidate is infeasible L0");
    count(first.nx0, expected_x, "budget candidate nx0");
    count(first.ny0, expected_y, "budget candidate ny0");
    std::clog << "asm mesh chain: rejected " << nx << 'x' << ny << " nz=" << 3 * n_elem_min << " ["
              << grid.error().path << "] " << grid.error().reason << '\n';
}

// Recursive, ordered comparisons: Real bits include signed zero; no sorting or hashes.
// Strings/integers are exact, and nested coordinate arrays retain every dimension.
template <class T>
void same(const T& a, const T& b, const std::string& field) {
    if constexpr (std::is_same_v<T, Real>) {
        check(std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b), field);
    } else if constexpr (requires {
                             a.first;
                             a.second;
                         }) {
        same(a.first, b.first, field + ".key");
        same(a.second, b.second, field + ".value");
    } else if constexpr (requires {
                             a.size();
                             a.begin();
                         }) {
        count(a.size(), b.size(), field + " length");
        auto other = b.begin();
        std::size_t i = 0;
        for (const auto& item : a) {
            same(item, *other, field + "[" + std::to_string(i++) + "]");
            ++other;
        }
    } else {
        check(a == b, field);
    }
}

void same_mesh(const HexMeshArrays& a, const HexMeshArrays& b) {
#define SAME(field) same(a.field, b.field, "mesh." #field)
    SAME(vertices);
    SAME(hex);
    SAME(attribute);
    SAME(elem_gid);
    SAME(axes);
    SAME(pending_cut);
#undef SAME
}

void same_polyhedra(const CutPolyhedra& a, const CutPolyhedra& b) {
#define SAME(field) same(a.field, b.field, "polyhedra." #field)
    SAME(points);
    SAME(point_xi);
    SAME(point_parent_elem);
    SAME(face_conn);
    SAME(face_off);
    SAME(poly_faces);
    SAME(poly_off);
    SAME(parent_elem_gid);
    SAME(body);
    SAME(layer_id);
    SAME(material_id);
    SAME(cell_kind);
    SAME(volume);
    SAME(volfrac_ref);
#undef SAME
}

void same_cut(const CutLayerArrays& a, const CutLayerArrays& b) {
#define SAME(field) same(a.field, b.field, "cut." #field)
    SAME(cut_elem);
    SAME(sub_off);
    SAME(sub_attribute);
    SAME(sub_volfrac);
    SAME(vq_off);
    SAME(bq_off);
    SAME(vq_xi);
    SAME(bq_xi);
    SAME(bq_n);
    SAME(vq_w);
    SAME(bq_w);
    SAME(bq_bdr_attr);
    SAME(gp_elem_gid);
    SAME(gp_face);
    SAME(gp_face_gid);
    SAME(gp_face_minus);
    SAME(gp_face_plus);
    SAME(gp_owner_rank);
    SAME(face_sets);
    SAME(eb_points);
    SAME(eb_xi);
    SAME(eb_normals);
    SAME(eb_parent_elem);
    SAME(sq_off);
    SAME(sq_side);
    SAME(sq_xi);
    SAME(sq_w);
    SAME(void_attribute);
    SAME(cut_elem_gid);
    SAME(void_elem);
    SAME(small_subs);
    SAME(exterior_area_vector);
#undef SAME
    count(a.level_sets.size(), b.level_sets.size(), "level set count");
    for (std::size_t i = 0; i < a.level_sets.size(); ++i) {
        same(a.level_sets[i].body, b.level_sets[i].body, "level set body");
        same(a.level_sets[i].phi, b.level_sets[i].phi, "level set phi");
    }
    same(a.polyhedra.has_value(), b.polyhedra.has_value(), "optional polyhedra presence");
    if (a.polyhedra)
        same_polyhedra(*a.polyhedra, *b.polyhedra);
}
} // namespace

int main() {
    return sp5::run([] {
        const Options options;
        std::array<Real, 2> lids{};
        for (const auto& scale : scales) {
            const auto run = pipeline(options, scale);
            if (scale.n == 2)
                lids[0] = body_volume(run, "lid");
            if (scale.n == 4)
                lids[1] = body_volume(run, "lid");
        }
        volume(lids[0], lids[1], 1e-12, "lid resolution 2/4");
        budget(options, 1, 56, 44, 630, 972);
        // Also lock the measured nz=6 table's successful lower resolutions.
        for (std::size_t i = 0; i < 3; ++i)
            (void)pipeline(options, scales[i], 2);
        budget(options, 2, 8, 8, 92, 180);
        budget(options, 2, 56, 44, 630, 972);
        const auto first = pipeline(options, scales[1]);
        const auto second = pipeline(options, scales[1]);
        same_mesh(first.before, second.before);
        same_mesh(first.after, second.after);
        same_cut(first.cut, second.cut);
    });
}
