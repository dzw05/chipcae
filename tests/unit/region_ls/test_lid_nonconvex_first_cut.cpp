#include "chipcae/mesh/cut/cut_stats.hpp"
#include "chipcae/mesh/cut/outline_cut.hpp"
#include "chipcae/mesh/cut/region_cut.hpp"
#include "support.hpp"

#include <bit>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <sys/resource.h>
#include <type_traits>
#include <utility>

namespace {
using chipcae::BodyId;
using chipcae::Real;
using namespace chipcae::mesh;
using namespace chipcae::mesh::cut;

void check(bool ok, const std::string& message) {
    sp5::require(ok, message.c_str());
}

template <class T>
T value(Expected<T> result, const std::string& stage) {
    if (!result)
        check(false, stage + " [" + result.error().path + "] " + result.error().reason);
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

// Hand-derived SI volumes: substrate .056*.044*.001;
// stiffener (.04*.04-.03*.03)*.0005;
// C lid (.002464-4*(.002*.002/2))*.0005 = .002456*.0005;
// N lid (.002456-(.008+.004)/2*.010)*.0005 = .002396*.0005.
constexpr std::array<Real, 3> n_volumes{2.464e-6, 3.5e-7, 1.198e-6};
constexpr std::array<Real, 3> c_volumes{2.464e-6, 3.5e-7, 1.228e-6};
constexpr std::array<Real, 3> box_volumes{2.464e-6, 3.5e-7, 1.232e-6};
constexpr std::array<const char*, 3> names{"substrate", "stiffener", "lid"};
// Independent expected normalized ring: CCW, starting at minimum (y,x).
constexpr std::array<std::array<Real, 2>, 12> expected_ring{{{.002, 0},
                                                             {.024, 0},
                                                             {.026, .010},
                                                             {.030, .010},
                                                             {.032, 0},
                                                             {.054, 0},
                                                             {.056, .002},
                                                             {.056, .042},
                                                             {.054, .044},
                                                             {.002, .044},
                                                             {0, .042},
                                                             {0, .002}}};

enum class Shape { N, C, Box };
StackModel stack(Shape shape = Shape::N, bool reverse = false) {
    StackModel s;
    s.id = "synthetic_notched_chamfer_lid";
    s.unit = "m";
    s.symmetry = "none";
    s.package_type = "assembly";
    s.outline.box = {0, 0, .056, .044};
    Layer substrate;
    substrate.name = "substrate";
    substrate.kind = "Explicit";
    substrate.material = "organic_eq";
    substrate.body = "substrate";
    substrate.thickness = .001;
    substrate.grading = 1;
    substrate.n_elem_min = 1;
    Layer stiffener;
    stiffener.name = "stiffener_layer";
    stiffener.kind = "Explicit";
    stiffener.thickness = .0005;
    stiffener.grading = 1;
    stiffener.n_elem_min = 1;
    stiffener.regions = {{"stiffener_south", "copper", "stiffener", {{.008, .002, .048, .007}, {}}},
                         {"stiffener_north", "copper", "stiffener", {{.008, .037, .048, .042}, {}}},
                         {"stiffener_west", "copper", "stiffener", {{.008, .007, .013, .037}, {}}},
                         {"stiffener_east", "copper", "stiffener", {{.043, .007, .048, .037}, {}}}};
    Layer lid;
    lid.name = "lid_layer";
    lid.kind = "Explicit";
    lid.thickness = .0005;
    lid.grading = 1;
    lid.n_elem_min = 1;
    Inclusion region{"lid", "copper", "lid", {{0, 0, .056, .044}, {}}};
    // Input is written independently of expected_ring.
    region.footprint.polygon = {{.002, 0},
                                {.024, 0},
                                {.026, .010},
                                {.030, .010},
                                {.032, 0},
                                {.054, 0},
                                {.056, .002},
                                {.056, .042},
                                {.054, .044},
                                {.002, .044},
                                {0, .042},
                                {0, .002}};
    if (shape == Shape::C)
        region.footprint.polygon = {{.002, 0},
                                    {.054, 0},
                                    {.056, .002},
                                    {.056, .042},
                                    {.054, .044},
                                    {.002, .044},
                                    {0, .042},
                                    {0, .002}};
    if (shape == Shape::Box)
        region.footprint.polygon.clear();
    if (reverse)
        std::reverse(region.footprint.polygon.begin(), region.footprint.polygon.end());
    lid.regions = {region};
    s.layers = {substrate, stiffener, lid};
    return s;
}

GridOptions grid_options(Local n) {
    GridOptions o;
    o.cell_budget = static_cast<chipcae::Index>(threshold("cell_budget"));
    o.snap_tol = threshold("snap_tol");
    o.max_ratio = threshold("max_ratio");
    o.nx = o.ny = n;
    o.levels = 1;
    o.ranks = 1;
    o.max_ratio = 2;
    o.snap_tol = 1e-14;
    o.cell_budget = 50000;
    return o;
}
CutOptions cut_options(bool export_polyhedra) {
    CutOptions o;
    o.quad_degree = 6;
    o.export_polyhedra = export_polyhedra;
    o.keep_void = false;
    o.snap_phi = threshold("snap_phi");
    o.small_sub_vf = threshold("small_sub_vf");
    return o;
}

// Recursive comparisons preserve ordering and floating representation, including signed zero.
template <class T>
void same(const T& a, const T& b, const char* field);
template <class T, std::size_t N>
void same(const std::array<T, N>& a, const std::array<T, N>& b, const char* field);
template <class T>
void same(const std::vector<T>& a, const std::vector<T>& b, const char* field);
template <class K, class V>
void same(const std::map<K, V>& a, const std::map<K, V>& b, const char* field);
template <class T>
void same(const T& a, const T& b, const char* field) {
    if constexpr (std::is_floating_point_v<T>)
        check(std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b),
              std::string("determinism bits: ") + field);
    else
        check(a == b, std::string("determinism: ") + field);
}
template <class T, std::size_t N>
void same(const std::array<T, N>& a, const std::array<T, N>& b, const char* field) {
    for (std::size_t i = 0; i < N; ++i)
        same(a[i], b[i], field);
}
template <class T>
void same(const std::vector<T>& a, const std::vector<T>& b, const char* field) {
    check(a.size() == b.size(), std::string("determinism length: ") + field);
    for (std::size_t i = 0; i < a.size(); ++i)
        same(a[i], b[i], field);
}
template <class K, class V>
void same(const std::map<K, V>& a, const std::map<K, V>& b, const char* field) {
    check(a.size() == b.size(), std::string("determinism map length: ") + field);
    auto j = b.begin();
    for (const auto& [key, v] : a) {
        same(key, j->first, field);
        same(v, j->second, field);
        ++j;
    }
}

BodyId body(const HexMeshArrays& mesh, Local attr) {
    check(attr > 0 && static_cast<std::size_t>(attr) <= mesh.attr_table.entries.size(),
          "attribute reference in range");
    return mesh.attr_table.entries[static_cast<std::size_t>(attr - 1)].body;
}
void offsets(const std::vector<Local>& off,
             std::size_t rows,
             std::size_t count,
             const char* field) {
    check(off.size() == rows + 1, std::string(field) + " CSR length");
    check(off.front() == 0 && std::is_sorted(off.begin(), off.end()) && off.back() >= 0 &&
              static_cast<std::size_t>(off.back()) == count,
          std::string(field) + " CSR bounds");
}
void validate(const HexMeshArrays& mesh, const CutLayerArrays& cut, bool polygon) {
    check(mesh.vertices.size() % 3 == 0, "complete XYZ vertices");
    check(mesh.hex.size() == 8 * mesh.attribute.size(), "connectivity length");
    check(mesh.node_gid.size() == mesh.vertices.size() / 3, "node GID length");
    check(mesh.elem_gid.size() == mesh.attribute.size(), "element GID length");
    for (auto node : mesh.hex)
        check(node >= 0 && static_cast<std::size_t>(node) < mesh.node_gid.size(), "node in range");
    for (auto attr : mesh.attribute)
        (void)body(mesh, attr);
    const auto lid = mesh.attr_table.bodies.at("lid");
    check(cut.cut_elem_gid.size() == cut.cut_elem.size(), "cut GID length");
    check(cut.sub_attribute.size() == cut.sub_volfrac.size(), "sub attribute length");
    offsets(cut.sub_off, cut.cut_elem.size(), cut.sub_volfrac.size(), "sub_off");
    offsets(cut.vq_off, cut.sub_volfrac.size(), cut.vq_w.size(), "vq_off");
    offsets(cut.bq_off, cut.sub_volfrac.size(), cut.bq_w.size(), "bq_off");
    for (std::size_t a = 0; a < 3; ++a) {
        check(cut.vq_xi[a].size() == cut.vq_w.size(), "volume quadrature length");
        check(cut.bq_xi[a].size() == cut.bq_w.size() && cut.bq_n[a].size() == cut.bq_w.size(),
              "boundary quadrature length");
    }
    std::array<std::size_t, 3> counts{};
    for (std::size_t row = 0; row < cut.cut_elem.size(); ++row) {
        const auto e = cut.cut_elem[row];
        check(e >= 0 && static_cast<std::size_t>(e) < mesh.attribute.size(), "cut parent in range");
        const auto index = static_cast<std::size_t>(e);
        same(cut.cut_elem_gid[row], mesh.elem_gid[index], "recorded parent GID");
        const auto id = body(mesh, mesh.attribute[index]);
        for (std::size_t i = 0; i < names.size(); ++i)
            counts[i] += id == mesh.attr_table.bodies.at(names[i]) ? 1U : 0U;
        check(id == lid, "every cut parent belongs to lid");
    }
    for (auto attr : cut.sub_attribute)
        check(body(mesh, attr) == lid, "every retained subcell belongs to lid");
    for (auto vf : cut.sub_volfrac)
        check(std::isfinite(vf) && vf > 0 && vf <= 1, "finite positive retained fraction <= 1");
    check(counts[0] == 0 && counts[1] == 0, "substrate/stiffener cut counts zero");
    check(polygon ? counts[2] > 0 : counts[2] == 0, "lid cut count matches polygon presence");
    check(cut.void_elem.empty(), "no retained void cells");
}

void relative(Real actual, Real expected, Real tolerance, const std::string& label) {
    const auto error = std::abs(actual - expected) / std::abs(expected);
    std::ostringstream message;
    message << std::setprecision(17) << label << " actual=" << actual << " expected=" << expected
            << " relative_error=" << error << " tolerance=" << tolerance;
    check(std::isfinite(actual) && error <= tolerance, message.str());
}
struct Run {
    HexMeshArrays mesh;
    CutLayerArrays cut;
    CutStats stats;
};

Run pipeline(const StackModel& s, Local n, Shape shape, bool export_polyhedra = true) {
    auto bounding = s;
    bounding.outline.polygon.clear(); // Preserve the lid region polygon, as in #201.
    auto mesh = value(build_tensor_grid(bounding, grid_options(n)), "build_tensor_grid");
    for (std::size_t a = 0; a < 2; ++a) {
        const auto& axis = mesh.axes[a];
        check(axis.size() >= 2, "XY axis has corner intervals");
        for (const auto width : {axis[1] - axis.front(), axis.back() - axis[axis.size() - 2]}) {
            std::clog << "corner axis=" << a << " width=" << std::setprecision(17) << width << '\n';
            check(std::isfinite(width) && width > 0, "finite positive corner widths before gates");
        }
    }
    for (auto name : names)
        check(mesh.attr_table.bodies.contains(name), std::string("body key: ") + name);
    const auto lid = mesh.attr_table.bodies.at("lid");
    const auto original_gids = mesh.elem_gid;
    const auto geometry = value(region_cut_planes(s, mesh), "region_cut_planes");
    if (shape == Shape::N) {
        check(geometry.planes.empty(), "N emits no planes for any body");
        check(geometry.unions.size() == 1, "N emits exactly one union");
        check(geometry.unions.front().body == lid, "N union belongs to lid");
        const auto& ring = geometry.unions.front().ring;
        check(ring.size() == expected_ring.size(), "N normalized ring has twelve vertices");
        for (std::size_t i = 0; i < ring.size(); ++i)
            same(ring[i], expected_ring[i], "independent normalized CCW ring");
    } else {
        check(geometry.unions.empty(), "control has no unions");
        check(geometry.planes.size() == (shape == Shape::C ? 8U : 0U), "control plane count");
        for (const auto& plane : geometry.planes)
            check(plane.body == lid, "only lid emits control planes");
    }
    auto result = value(apply_outline_cut(mesh, s.outline, cut_options(export_polyhedra), geometry),
                        "apply_outline_cut");
    check(result.has_value() == (shape != Shape::Box), "cut optional matches polygon presence");
    auto cut = result ? std::move(*result) : CutLayerArrays{};
    validate(mesh, cut, shape != Shape::Box);
    auto next = original_gids.begin();
    for (auto gid : mesh.elem_gid) {
        next = std::find(next, original_gids.end(), gid);
        check(next != original_gids.end(), "post-cut GIDs preserve original order");
        ++next;
    }
    const auto stats = value(cut_stats(mesh, cut), "cut_stats");
    check(stats.total_cells == mesh.attribute.size() && stats.cut_cells == cut.cut_elem.size() &&
              stats.sub_cells == cut.sub_volfrac.size() && stats.void_cells == cut.void_elem.size(),
          "stats match array lengths");
    check(stats.full_cells + stats.cut_cells + stats.void_cells == stats.total_cells,
          "full + cut + void == total");
    check(stats.small_subs_recorded == cut.small_subs.size(), "small_subs recorded count");
    for (auto sub : cut.small_subs) {
        check(sub >= 0 && static_cast<std::size_t>(sub) < cut.sub_volfrac.size(),
              "small index valid");
        check(cut.sub_volfrac[static_cast<std::size_t>(sub)] < threshold("small_sub_vf"),
              "recorded small fraction strictly below threshold");
    }
    for (std::size_t bin = 0; bin < kSmallVfBins.size(); ++bin) {
        const auto direct = std::count_if(cut.sub_volfrac.begin(),
                                          cut.sub_volfrac.end(),
                                          [&](Real vf) { return vf < kSmallVfBins[bin]; });
        check(stats.small_vf_cumulative[bin] == static_cast<std::size_t>(direct), "cumulative bin");
    }
    check(stats.min_sub_volfrac.has_value() == !cut.sub_volfrac.empty(), "minimum presence");
    if (stats.min_sub_volfrac)
        same(*stats.min_sub_volfrac,
             *std::min_element(cut.sub_volfrac.begin(), cut.sub_volfrac.end()),
             "minimum fraction");
    std::clog << "实测 n=" << n << " cut=" << stats.cut_cells
              << " small_subs=" << cut.small_subs.size() << " min_sub_volfrac=";
    if (stats.min_sub_volfrac)
        std::clog << *stats.min_sub_volfrac;
    else
        std::clog << "none";
    std::clog << '\n';
    const auto& expected = shape == Shape::N   ? n_volumes
                           : shape == Shape::C ? c_volumes
                                               : box_volumes;
    check(stats.body_volume.size() == names.size(), "exactly three body volume keys");
    for (std::size_t i = 0; i < names.size(); ++i) {
        const auto id = mesh.attr_table.bodies.at(names[i]);
        check(stats.body_volume.contains(id), "body volume key exists");
        std::clog << "实测 body=" << names[i] << " volume=" << stats.body_volume.at(id) << '\n';
        relative(stats.body_volume.at(id), expected[i], threshold("cut_volume_relative"), names[i]);
    }
    relative(stats.body_volume.at(lid), expected[2], 1e-12, "lid strict volume");
    relative(stats.solid_volume,
             shape == Shape::N   ? 4.012e-6
             : shape == Shape::C ? 4.042e-6
                                 : 4.046e-6,
             threshold("cut_volume_relative"),
             "solid volume");
    return {std::move(mesh), std::move(cut), stats};
}

// Independent normals from the original CCW ring reject decomposition caps.
void surfaces(const Run& run) {
    const auto& mesh = run.mesh;
    const auto& cut = run.cut;
    offsets(cut.sq_off, cut.cut_elem.size(), cut.sq_w.size(), "sq_off");
    check(cut.sq_side.size() == cut.sq_w.size(), "surface sides length");
    for (const auto& xi : cut.sq_xi)
        check(xi.size() == cut.sq_w.size(), "surface xi length");
    check(cut.exterior_area_vector.size() == cut.cut_elem.size(), "exterior vector length");
    Real embedded_area = 0;
    const Real tolerance = threshold("closure_relative");
    const Real point_tolerance = 16 * threshold("snap_phi");
    for (std::size_t row = 0; row < cut.cut_elem.size(); ++row) {
        const auto e = static_cast<std::size_t>(cut.cut_elem[row]);
        std::array<Real, 3> lo, hi;
        lo.fill(std::numeric_limits<Real>::infinity());
        hi.fill(-std::numeric_limits<Real>::infinity());
        for (std::size_t v = 0; v < 8; ++v)
            for (std::size_t a = 0; a < 3; ++a) {
                const auto p = mesh.vertices[3 * static_cast<std::size_t>(mesh.hex[8 * e + v]) + a];
                lo[a] = std::min(lo[a], p);
                hi[a] = std::max(hi[a], p);
            }
        const auto axy = (hi[0] - lo[0]) * (hi[1] - lo[1]);
        check(std::isfinite(axy) && axy > 0, "positive parent characteristic area");
        std::array<Real, 3> parent{}, embedded{};
        for (auto q = static_cast<std::size_t>(cut.sq_off[row]);
             q < static_cast<std::size_t>(cut.sq_off[row + 1]);
             ++q) {
            const auto side = cut.sq_side[q];
            check(side >= -1 && side <= 5, "surface side in [-1,5]");
            check(std::isfinite(cut.sq_w[q]) && cut.sq_w[q] > 0,
                  "positive physical surface weight");
            std::array<Real, 3> p{}, normal{};
            for (std::size_t a = 0; a < 3; ++a) {
                check(std::isfinite(cut.sq_xi[a][q]), "finite surface reference coordinate");
                p[a] = lo[a] + cut.sq_xi[a][q] * (hi[a] - lo[a]);
            }
            if (side >= 0) {
                const auto a = static_cast<std::size_t>(side / 2);
                normal[a] = side % 2 ? 1 : -1;
                check(std::abs(p[a] - (side % 2 ? hi[a] : lo[a])) <= point_tolerance,
                      "parent surface point lies on coded side");
            } else {
                std::size_t matches = 0;
                for (std::size_t edge = 0; edge < expected_ring.size(); ++edge) {
                    const auto& a = expected_ring[edge];
                    const auto& b = expected_ring[(edge + 1) % expected_ring.size()];
                    const bool box_edge = (a[0] == b[0] && (a[0] == 0 || a[0] == .056)) ||
                                          (a[1] == b[1] && (a[1] == 0 || a[1] == .044));
                    if (box_edge)
                        continue;
                    const Real dx = b[0] - a[0], dy = b[1] - a[1];
                    const Real length = std::hypot(dx, dy);
                    const Real distance =
                        std::abs(dx * (p[1] - a[1]) - dy * (p[0] - a[0])) / length;
                    const Real along = ((p[0] - a[0]) * dx + (p[1] - a[1]) * dy) / length;
                    if (distance <= point_tolerance && along >= -point_tolerance &&
                        along <= length + point_tolerance) {
                        ++matches;
                        normal = {dy / length, -dx / length, 0};
                    }
                }
                check(matches == 1,
                      "embedded point lies on exactly one original non-box edge; no cap");
                check(p[2] >= lo[2] - point_tolerance && p[2] <= hi[2] + point_tolerance,
                      "embedded point within parent height");
                embedded_area += cut.sq_w[q];
            }
            auto& sum = side >= 0 ? parent : embedded;
            for (std::size_t a = 0; a < 3; ++a)
                sum[a] += cut.sq_w[q] * normal[a];
        }
        std::array<Real, 3> difference{}, boundary{};
        for (std::size_t a = 0; a < 3; ++a) {
            difference[a] = parent[a] - cut.exterior_area_vector[row][a];
            boundary[a] = parent[a] + embedded[a];
        }
        // exterior_area_vector stores only surviving parent-face vectors (plane_cut.cpp).
        // The full oriented boundary, including embedded faces, must instead sum to zero.
        check(std::hypot(difference[0], difference[1], difference[2]) / axy <= tolerance,
              "parent-face vector agrees with exterior_area_vector row=" + std::to_string(row));
        check(std::hypot(boundary[0], boundary[1], boundary[2]) / axy <= tolerance,
              "whole-boundary closure row=" + std::to_string(row));
    }
    const auto expected_area =
        (4 * std::sqrt(2.) * .002 + 2 * std::hypot(.002, .010) + .004) * .0005;
    std::clog << "实测 embedded_area=" << embedded_area << " expected=" << expected_area << '\n';
    relative(embedded_area, expected_area, 1e-12, "embedded physical area");
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

Real peak_rss_mib() {
    struct rusage usage{};
    check(getrusage(RUSAGE_SELF, &usage) == 0, "getrusage succeeds");
#if defined(__APPLE__)
    return static_cast<Real>(usage.ru_maxrss) / (1024 * 1024);
#else
    return static_cast<Real>(usage.ru_maxrss) / 1024;
#endif
}
} // namespace

int main() {
    return sp5::run([] {
        std::array<Real, 2> lid_volumes{};
        for (std::size_t resolution = 0; resolution < 2; ++resolution) {
            const Local n = resolution == 0 ? 2 : 4;
            std::string context = "N n=" + std::to_string(n) + " primary";
            try {
                std::clog << "case " << context << '\n';
                const auto start = std::chrono::steady_clock::now();
                const auto first = pipeline(stack(), n, Shape::N);
                const auto seconds =
                    std::chrono::duration<Real>(std::chrono::steady_clock::now() - start).count();
                std::clog << "MEASURE lid_nonconvex_first_cut n=" << n << " seconds=" << seconds
                          << " peak_rss_MiB=" << peak_rss_mib()
                          << " cells=" << first.stats.total_cells
                          << " cut=" << first.stats.cut_cells << '\n';
                surfaces(first);
                lid_volumes[resolution] =
                    first.stats.body_volume.at(first.mesh.attr_table.bodies.at("lid"));
                context = "N n=" + std::to_string(n) + " repeat";
                {
                    const auto repeated = pipeline(stack(), n, Shape::N);
                    deterministic(first, repeated);
                }
                context = "N n=" + std::to_string(n) + " clockwise";
                {
                    const auto reversed = pipeline(stack(Shape::N, true), n, Shape::N);
                    deterministic(first, reversed);
                }
                context = "N n=" + std::to_string(n) + " export requested false";
                {
                    // apply_outline_cut currently forces export on: this verifies request
                    // invariance, and cannot establish that an export-disabled implementation was
                    // executed.
                    const auto without_export = pipeline(stack(), n, Shape::N, false);
                    same(first.stats.body_volume,
                         without_export.stats.body_volume,
                         "export body volumes");
                }
                context = "C n=" + std::to_string(n);
                {
                    const auto control = pipeline(stack(Shape::C), n, Shape::C);
                }
                context = "Box n=" + std::to_string(n);
                {
                    const auto control = pipeline(stack(Shape::Box), n, Shape::Box);
                }
            } catch (const std::exception& error) {
                throw std::runtime_error(context + ": " + error.what());
            }
        }
        check(std::abs(lid_volumes[0] - lid_volumes[1]) / 1.198e-6 <= 1e-12,
              "N n=2/4 lid resolution independence");
    });
}
