#include "chipcae/mesh/background/tensor_grid.hpp"
#include "chipcae/mesh/cut/cut_stats.hpp"
#include "chipcae/mesh/cut/outline_cut.hpp"
#include "chipcae/mesh/cut/region_cut.hpp"
#include "chipcae/schema/stack.hpp"
#include "support.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
using chipcae::BodyId;
using chipcae::Real;
using namespace chipcae::mesh;
using namespace chipcae::mesh::cut;

// Synthetic geometry from STEP parts contract case 7b, expressed in SI metres.
// This fixture contains no customer geometry. It follows stack_writer's assembly shape.
// Analytic volumes: substrate .056*.044*.001; ring (.04^2-.03^2)*.0005;
// lid (.056*.044-2*.002^2)*.0005. No measured values define expectations.
constexpr std::array<Real, 3> chamfer_volumes{2.464e-6, 3.5e-7, 1.228e-6};
constexpr std::array<Real, 3> rectangular_volumes{2.464e-6, 3.5e-7, 1.232e-6};
constexpr std::array<const char*, 3> body_names{"substrate", "stiffener", "lid"};

void check(bool condition, const std::string& message) {
    k1::expect(condition, ("asm ring-lid: " + message).c_str());
}

template <class T>
T value(Expected<T> result, const std::string& message) {
    if (!result)
        check(false, message + " [" + result.error().path + "] " + result.error().reason);
    return std::move(result.value());
}

StackModel load() {
    const std::filesystem::path root{K1_ROOT};
    return value(load_stack(root / "tests/fixtures/mesh/step_ring_chamfer_lid.stack.json",
                            root / "schema/stack-0.2.schema.json"),
                 "load synthetic assembly");
}

Inclusion& lid_region(StackModel& stack) {
    for (auto& layer : stack.layers)
        if (layer.name == "lid_layer")
            for (auto& region : layer.regions)
                if (region.name == "lid")
                    return region;
    check(false, "fixture has lid_layer/lid");
    throw std::runtime_error("asm ring-lid: missing lid");
}

GridOptions options(Local n) {
    auto o = k1::options();
    o.nx = o.ny = n;
    o.levels = 1;
    o.ranks = 1;
    o.max_ratio = 2;
    o.snap_tol = 1e-14;
    o.cell_budget = 50000;
    return o;
}

CutOptions cut_options() {
    CutOptions o;
    o.quad_degree = 6;
    o.export_polyhedra = false;
    o.keep_void = false;
    o.snap_phi = k1::threshold("snap_phi");
    o.small_sub_vf = k1::threshold("small_sub_vf");
    return o;
}

void corners(const HexMeshArrays& mesh, const GridOptions& o) {
    const auto& x = mesh.axes[0];
    const auto& y = mesh.axes[1];
    check(x.size() >= 2 && y.size() >= 2, "XY axes each have at least two entries");
    std::clog << std::setprecision(17) << "asm ring-lid: 实测 GridOptions nx=" << o.nx
              << " ny=" << o.ny << " levels=" << o.levels << " ranks=" << o.ranks
              << " max_ratio=" << o.max_ratio << " snap_tol=" << o.snap_tol
              << " cell_budget=" << o.cell_budget << " ar_warn=" << o.ar_warn
              << " ar_max=" << o.ar_max << " use_metis=" << o.use_metis << '\n';
    const std::array hx{x[1] - x.front(), x.back() - x[x.size() - 2]};
    const std::array hy{y[1] - y.front(), y.back() - y[y.size() - 2]};
    bool valid = true;
    for (std::size_t i = 0; i < 2; ++i)
        for (std::size_t j = 0; j < 2; ++j) {
            std::clog << "asm ring-lid: corner " << (i ? "east" : "west") << '/'
                      << (j ? "north" : "south") << " hx=" << hx[i] << " hy=" << hy[j] << '\n';
            valid = valid && std::isfinite(hx[i]) && hx[i] > 0 && std::isfinite(hy[j]) && hy[j] > 0;
        }
    check(valid, "all four corner widths must be finite and strictly positive; stopping run");
}

HexMeshArrays grid(const StackModel& stack, Local n) {
    auto bounding_stack = stack;
    bounding_stack.outline.polygon.clear();
    const auto o = options(n);
    auto mesh = value(build_tensor_grid(bounding_stack, o), "build bounding tensor grid");
    corners(mesh, o); // Must precede planes and all volume assertions.
    return mesh;
}

std::array<BodyId, 3> bodies(const HexMeshArrays& mesh) {
    std::array<BodyId, 3> ids{};
    for (std::size_t i = 0; i < ids.size(); ++i) {
        const auto it = mesh.attr_table.bodies.find(body_names[i]);
        check(it != mesh.attr_table.bodies.end(), std::string("missing body ") + body_names[i]);
        ids[i] = it->second;
    }
    check(ids[0] != ids[1] && ids[0] != ids[2] && ids[1] != ids[2], "body IDs are distinct");
    return ids;
}

BodyId body(const HexMeshArrays& mesh, Local attr) {
    check(attr > 0 && static_cast<std::size_t>(attr) <= mesh.attr_table.entries.size(),
          "attribute reference is in range");
    return mesh.attr_table.entries[static_cast<std::size_t>(attr - 1)].body;
}

void validate_mesh(const HexMeshArrays& mesh) {
    check(mesh.vertices.size() % 3 == 0, "vertices contain complete XYZ triples");
    check(mesh.hex.size() == 8 * mesh.attribute.size(), "hex connectivity length");
    check(mesh.elem_gid.size() == mesh.attribute.size(), "element GID length");
    for (const auto node : mesh.hex)
        check(node >= 0 && static_cast<std::size_t>(node) < mesh.vertices.size() / 3,
              "hex node index is in range");
    for (const auto attr : mesh.attribute)
        (void)body(mesh, attr);
}

void offsets(const std::vector<Local>& off, std::size_t rows, std::size_t count, const char* name) {
    check(off.size() == rows + 1, std::string(name) + " CSR length");
    check(off.front() == 0 && std::is_sorted(off.begin(), off.end()) && off.back() >= 0 &&
              static_cast<std::size_t>(off.back()) == count,
          std::string(name) + " CSR bounds");
}

void validate_cut(const HexMeshArrays& mesh,
                  const CutLayerArrays& cut,
                  const std::array<BodyId, 3>& ids,
                  bool chamfered) {
    validate_mesh(mesh);
    check(cut.cut_elem_gid.size() == cut.cut_elem.size(), "cut parent GID length");
    check(cut.sub_attribute.size() == cut.sub_volfrac.size(), "subcell attribute length");
    offsets(cut.sub_off, cut.cut_elem.size(), cut.sub_volfrac.size(), "sub_off");
    offsets(cut.vq_off, cut.sub_volfrac.size(), cut.vq_w.size(), "vq_off");
    std::array<std::size_t, 3> counts{};
    constexpr std::array<std::array<Real, 4>, 4> influence{{{0, 0, .002, .002},
                                                            {.054, 0, .056, .002},
                                                            {0, .042, .002, .044},
                                                            {.054, .042, .056, .044}}};
    for (std::size_t row = 0; row < cut.cut_elem.size(); ++row) {
        const auto local = cut.cut_elem[row];
        check(local >= 0 && static_cast<std::size_t>(local) < mesh.attribute.size(),
              "cut parent index is in range");
        const auto e = static_cast<std::size_t>(local);
        check(mesh.elem_gid[e] == cut.cut_elem_gid[row], "cut index resolves to recorded GID");
        const auto id = body(mesh, mesh.attribute[e]);
        for (std::size_t i = 0; i < ids.size(); ++i)
            counts[i] += id == ids[i] ? 1 : 0;
        check(id == ids[2], "every cut parent belongs to lid");
        std::array<Real, 2> lo{1, 1}, hi{-1, -1};
        for (std::size_t node = 0; node < 8; ++node) {
            const auto v = 3 * static_cast<std::size_t>(mesh.hex[8 * e + node]);
            for (std::size_t axis = 0; axis < 2; ++axis) {
                lo[axis] = std::min(lo[axis], mesh.vertices[v + axis]);
                hi[axis] = std::max(hi[axis], mesh.vertices[v + axis]);
            }
        }
        const bool overlaps = std::ranges::any_of(influence, [&](const auto& box) {
            return std::min(hi[0], box[2]) > std::max(lo[0], box[0]) &&
                   std::min(hi[1], box[3]) > std::max(lo[1], box[1]);
        });
        check(overlaps, "cut parent has positive XY overlap with a chamfer influence box");
    }
    for (std::size_t sub = 0; sub < cut.sub_attribute.size(); ++sub) {
        check(body(mesh, cut.sub_attribute[sub]) == ids[2],
              "every retained subcell belongs to lid");
    }
    check(counts[0] == 0 && counts[1] == 0, "substrate and stiffener cut counts are zero");
    check(chamfered ? counts[2] > 0 : counts[2] == 0, "lid cut count matches polygon presence");
    check(cut.void_elem.empty(), "mutable cut removes voids");
}

void volume(Real actual, Real expected, const std::string& label) {
    const auto relative = std::abs(actual - expected) / expected;
    std::clog << "asm ring-lid: 实测 " << label << " volume=" << actual << " expected=" << expected
              << " relative_error=" << relative << '\n';
    check(std::isfinite(actual) &&
              std::abs(actual - expected) <= k1::threshold("cut_volume_relative") * expected,
          label + " analytic volume gate");
}

struct Run {
    HexMeshArrays mesh;
    CutLayerArrays cut;
    CutStats stats;
};

Run pipeline(const StackModel& stack, Local n, bool chamfered = true) {
    auto mesh = grid(stack, n);
    const auto ids = bodies(mesh);
    if (chamfered)
        check(!mesh.pending_cut.empty(), "polygon input has pending_cut evidence for SP6-B");
    const auto original_gids = mesh.elem_gid;
    const auto planes = value(region_cut_planes(stack, mesh), "region_cut_planes");
    check(planes.planes.size() == (chamfered ? 8U : 0U), "region plane count");
    for (const auto& plane : planes.planes)
        check(plane.body == ids[2], "only lid emits region planes");
    auto result =
        value(apply_outline_cut(mesh, stack.outline, cut_options(), planes), "apply_outline_cut");
    check(result.has_value() == chamfered, "cut optional matches polygon presence");
    auto cut = result ? std::move(*result) : CutLayerArrays{};
    validate_cut(mesh, cut, ids, chamfered);
    const auto stats = value(cut_stats(mesh, cut), "cut_stats");
    check(stats.total_cells == mesh.attribute.size() && stats.cut_cells == cut.cut_elem.size() &&
              stats.sub_cells == cut.sub_volfrac.size() &&
              stats.void_cells == cut.void_elem.size() &&
              stats.small_subs_recorded == cut.small_subs.size(),
          "stats counts match mesh and cut array lengths");
    check(stats.full_cells + stats.cut_cells + stats.void_cells == stats.total_cells,
          "full + cut + void == total");
    auto next = original_gids.begin();
    for (const auto gid : mesh.elem_gid) {
        next = std::find(next, original_gids.end(), gid);
        check(next != original_gids.end(), "post-cut elem_gid is a preserving-order subsequence");
        ++next;
    }
    std::clog << "asm ring-lid: 实测 nx=" << n << " chamfered=" << chamfered
              << " cells=" << stats.total_cells << " full=" << stats.full_cells
              << " cut=" << stats.cut_cells << " sub=" << stats.sub_cells
              << " void=" << stats.void_cells << '\n';
    const auto& expected = chamfered ? chamfer_volumes : rectangular_volumes;
    for (std::size_t i = 0; i < ids.size(); ++i) {
        const auto it = stats.body_volume.find(ids[i]);
        check(it != stats.body_volume.end(), std::string("missing volume key ") + body_names[i]);
        volume(it->second, expected[i], body_names[i]);
    }
    volume(stats.solid_volume, chamfered ? 4.042e-6 : 4.046e-6, "solid");
    return {std::move(mesh), std::move(cut), stats};
}

// Compare lengths before elements; real values compare by representation, including signed zero.
template <class T>
void same(const T& a, const T& b, const char* field) {
    if constexpr (std::is_floating_point_v<T>)
        check(std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b),
              std::string("determinism bits: ") + field);
    else
        check(a == b, std::string("determinism: ") + field);
}
template <class T, std::size_t N>
void same(const std::array<T, N>& a, const std::array<T, N>& b, const char* field);
template <class T>
void same(const std::vector<T>& a, const std::vector<T>& b, const char* field) {
    check(a.size() == b.size(), std::string("determinism length: ") + field);
    for (std::size_t i = 0; i < a.size(); ++i)
        same(a[i], b[i], field);
}
template <class T, std::size_t N>
void same(const std::array<T, N>& a, const std::array<T, N>& b, const char* field) {
    for (std::size_t i = 0; i < N; ++i)
        same(a[i], b[i], field);
}

void deterministic(const Run& first, const Run& second) {
    {
        const auto& a = first.mesh;
        const auto& b = second.mesh;
#define SAME(field) same(a.field, b.field, #field)
        SAME(vertices);
        SAME(hex);
        SAME(attribute);
        SAME(bdr_quad);
        SAME(bdr_attribute);
        SAME(elem_gid);
        SAME(node_gid);
        SAME(partitioning);
        SAME(node_layer);
        SAME(copper_layer_of_layer);
        SAME(copper_phi);
        SAME(node_column);
        SAME(attr_frame);
        SAME(nodes);
        SAME(internal_interfaces);
        SAME(order);
        SAME(face_sets);
        SAME(pending_cut);
        SAME(axes);
        SAME(attr_table.layers);
        SAME(attr_table.references);
        SAME(attr_table.materials);
        SAME(attr_table.bodies);
        SAME(attr_table.boundary_names);
        check(a.attr_table.entries.size() == b.attr_table.entries.size(), "attribute table length");
        for (std::size_t i = 0; i < a.attr_table.entries.size(); ++i) {
            const auto& x = a.attr_table.entries[i];
            const auto& y = b.attr_table.entries[i];
            same(x.layer, y.layer, "entry.layer");
            same(x.region, y.region, "entry.region");
            same(x.material, y.material, "entry.material");
            same(x.body, y.body, "entry.body");
        }
        SAME(grid_report.levels);
        SAME(grid_report.rule);
        SAME(grid_report.N_L);
        SAME(grid_report.auto_selected);
        SAME(grid_report.padding);
        SAME(grid_report.max_aspect_ratio);
        SAME(grid_report.level_axes);
        SAME(grid_report.column_ranks);
        SAME(grid_report.warnings);
        SAME(grid_report.partition_imbalance);
        SAME(grid_report.void_cells);
        check(a.snapped.size() == b.snapped.size(), "snap report length");
        for (std::size_t i = 0; i < a.snapped.size(); ++i) {
            const auto& x = a.snapped[i];
            const auto& y = b.snapped[i];
            same(x.axis, y.axis, "snap.axis");
            same(x.from, y.from, "snap.from");
            same(x.to, y.to, "snap.to");
            same(x.displacement, y.displacement, "snap.displacement");
        }
        check(a.preparation_report.size() == b.preparation_report.size(),
              "interface report length");
        for (std::size_t i = 0; i < a.preparation_report.size(); ++i) {
            const auto& x = a.preparation_report[i];
            const auto& y = b.preparation_report[i];
            same(x.name, y.name, "interface.name");
            same(x.side_a_bodies, y.side_a_bodies, "interface.side_a_bodies");
            same(x.side_b_bodies, y.side_b_bodies, "interface.side_b_bodies");
            same(x.aligned_faces, y.aligned_faces, "interface.aligned_faces");
            same(x.cut_faces, y.cut_faces, "interface.cut_faces");
            same(x.aligned_area, y.aligned_area, "interface.aligned_area");
            same(x.cut_area, y.cut_area, "interface.cut_area");
        }
#undef SAME
    }
    {
        const auto& a = first.cut;
        const auto& b = second.cut;
#define SAME(field) same(a.field, b.field, #field)
        same(a.polyhedra.has_value(), b.polyhedra.has_value(), "polyhedra presence");
        if (a.polyhedra) {
            const auto& x = *a.polyhedra;
            const auto& y = *b.polyhedra;
#define POLY(field) same(x.field, y.field, "polyhedra." #field)
            POLY(points);
            POLY(point_xi);
            POLY(point_parent_elem);
            POLY(face_conn);
            POLY(face_off);
            POLY(poly_faces);
            POLY(poly_off);
            POLY(parent_elem_gid);
            POLY(body);
            POLY(layer_id);
            POLY(material_id);
            POLY(cell_kind);
            POLY(volume);
            POLY(volfrac_ref);
#undef POLY
        }
        SAME(cut_elem);
        SAME(cut_elem_gid);
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
        SAME(void_elem);
        SAME(small_subs);
        SAME(exterior_area_vector);
        check(a.level_sets.size() == b.level_sets.size(), "level set count");
        for (std::size_t i = 0; i < a.level_sets.size(); ++i) {
            same(a.level_sets[i].body, b.level_sets[i].body, "level set body");
            same(a.level_sets[i].phi, b.level_sets[i].phi, "level set phi");
        }
#undef SAME
    }
}

void material_layer_negative(Local n) {
    auto stack = load();
    const auto valid_mesh = grid(stack, n);
    const auto region = lid_region(stack);
    for (auto& layer : stack.layers)
        if (layer.name == "lid_layer")
            layer.regions.clear();
    check(!stack.layers.empty() && !stack.layers.front().material.empty(),
          "negative fixture has a material layer");
    stack.layers.front().regions.push_back(region);
    const auto result = region_cut_planes(stack, valid_mesh);
    check(!result, "polygon in material layer must fail");
    check(result.error().path == "CUT_UNSUPPORTED" &&
              result.error().reason.find("polygon in material layer") != std::string::npos,
          "material-layer rejection [" + result.error().path + "] " + result.error().reason);
    std::clog << "asm ring-lid: 实测 negative [" << result.error().path << "] "
              << result.error().reason << '\n';
}
} // namespace

int main(int argc, char** argv) {
#ifdef CHIPCAE_ENABLE_KOKKOS
    Kokkos::ScopeGuard guard(argc, argv);
#else
    (void)argc;
    (void)argv;
#endif
    int failures = 0;
    const auto run = [&](const std::string& name, auto test) {
        std::clog << "asm ring-lid: case " << name << '\n';
        try {
            test();
        } catch (const std::exception& error) {
            std::cerr << "asm ring-lid: " << name << ": " << error.what() << '\n';
            ++failures;
        }
    };
    std::array<std::optional<Real>, 2> lid_volumes;
    for (std::size_t i = 0; i < 2; ++i) {
        const Local n = i == 0 ? 2 : 4;
        const auto label = " nx=" + std::to_string(n);
        run("positive/repeat" + label, [&] {
            const auto first_stack = load();
            const auto second_stack = load();
            const auto first = pipeline(first_stack, n);
            const auto second = pipeline(second_stack, n);
            deterministic(first, second);
            lid_volumes[i] = first.stats.body_volume.at(bodies(first.mesh)[2]);
        });
        run("material-layer" + label, [&] { material_layer_negative(n); });
        run("clockwise" + label, [&] {
            auto stack = load();
            auto& polygon = lid_region(stack).footprint.polygon;
            std::reverse(polygon.begin(), polygon.end());
            (void)pipeline(stack, n);
            std::clog << "asm ring-lid: 实测 clockwise accepted with analytic volumes\n";
        });
        run("empty polygon" + label, [&] {
            auto stack = load();
            lid_region(stack).footprint.polygon.clear();
            (void)pipeline(stack, n, false);
        });
    }
    run("resolution independence", [&] {
        check(lid_volumes[0] && lid_volumes[1], "both positive resolutions completed");
        check(std::abs(*lid_volumes[0] - *lid_volumes[1]) <= 1e-12 * chamfer_volumes[2],
              "lid h/h2 volume difference is at most 1e-12 relative");
    });
    return failures == 0 ? 0 : 1;
}
