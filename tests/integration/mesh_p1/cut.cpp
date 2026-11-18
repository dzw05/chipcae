#include "chipcae/mesh/cut/outline_cut.hpp"
#include "chipcae/mesh/cut/plane_cut.hpp"
#include "support.hpp"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <set>
#include <sstream>
using namespace chipcae::mesh;
using namespace chipcae::mesh::cut;
using k1::expect;
// Independent analytic CDF of a positive weighted sum of three uniform variables.
long double exact(std::array<double, 3> n, double d) {
    long double sum = 0;
    for (int mask = 0; mask < 8; ++mask) {
        long double t = d;
        int sign = 1;
        for (int k = 0; k < 3; ++k)
            if (mask & (1 << k)) {
                t -= n[k];
                sign = -sign;
            }
        if (t > 0)
            sum += sign * t * t * t;
    }
    return sum / (6 * static_cast<long double>(n[0]) * n[1] * n[2]);
}
// Independent topology and signed-divergence checks on exported cells.
void check_poly(const CutPolyhedra& p, double snap) {
    using V = std::array<double, 3>;
    auto point = [&](chipcae::Index id) {
        return V{p.points[3 * id], p.points[3 * id + 1], p.points[3 * id + 2]};
    };
    auto sub = [](V a, V b) { return V{a[0] - b[0], a[1] - b[1], a[2] - b[2]}; };
    auto cross = [](V a, V b) {
        return V{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    };
    auto dot = [](V a, V b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    for (std::size_t i = 0; i < p.volume.size(); ++i) {
        std::map<std::pair<chipcae::Index, chipcae::Index>, int> edges;
        std::set<chipcae::Index> ids;
        for (auto f = p.poly_off[i]; f < p.poly_off[i + 1]; ++f) {
            auto face = p.poly_faces[f];
            for (auto j = p.face_off[face]; j < p.face_off[face + 1]; ++j)
                ids.insert(p.face_conn[j]);
        }
        V center{};
        for (auto id : ids) {
            auto x = point(id);
            for (int d = 0; d < 3; ++d)
                center[d] += x[d] / ids.size();
        }
        double volume = 0;
        for (auto a : ids)
            for (auto b : ids)
                if (a < b) {
                    auto delta = sub(point(a), point(b));
                    expect(dot(delta, delta) > snap * snap, "canonical points separated");
                }
        for (auto f = p.poly_off[i]; f < p.poly_off[i + 1]; ++f) {
            auto face = p.poly_faces[f], begin = p.face_off[face], end = p.face_off[face + 1];
            std::set<chipcae::Index> ring;
            V area{};
            auto a = point(p.face_conn[begin]);
            for (auto j = begin; j < end; ++j) {
                auto u = p.face_conn[j], v = p.face_conn[j + 1 == end ? begin : j + 1];
                expect(ring.insert(u).second, "simple face ring");
                ++edges[{u, v}];
            }
            for (auto j = begin + 1; j + 1 < end; ++j) {
                auto b = point(p.face_conn[j]), c = point(p.face_conn[j + 1]);
                auto n = cross(sub(b, a), sub(c, a));
                for (int d = 0; d < 3; ++d)
                    area[d] += n[d] / 2;
                volume += dot(sub(a, center), cross(sub(b, center), sub(c, center))) / 6;
            }
            expect(std::sqrt(dot(area, area)) >= snap * snap, "nondegenerate face");
            expect(dot(area, sub(a, center)) > 0, "outward face normal");
        }
        for (const auto& [edge, count] : edges)
            expect(count == 1 && edges[{edge.second, edge.first}] == 1, "opposite edge pairing");
        expect(volume > 0 && std::abs(volume - p.volume[i]) < 1e-12,
               "positive signed divergence volume");
        expect(std::abs(p.volume[i] - p.volfrac_ref[i]) < 1e-12,
               "export matches quadrature fraction");
    }
}

static void chamfer_lid_route() {
    using chipcae::BodyId;
    using chipcae::Real;
    auto stack = k1::stack();
    stack.activation_groups.clear();
    stack.outline = {{0, 0, 56, 44}, {}};
    const auto& [xmin, ymin, xmax, ymax] = stack.outline.box;
    stack.layers.resize(1);
    auto& substrate = stack.layers.front();
    substrate.name = "substrate";
    substrate.regions.clear();
    const auto material = substrate.material;

    Layer ring;
    ring.name = "ring_layer";
    ring.kind = "Explicit";
    ring.regions = {{"ring_s", material, "ring", {{10, 10, 46, 14}, {}}},
                    {"ring_n", material, "ring", {{10, 30, 46, 34}, {}}},
                    {"ring_w", material, "ring", {{10, 14, 14, 30}, {}}},
                    {"ring_e", material, "ring", {{42, 14, 46, 30}, {}}}};
    Layer lid;
    lid.name = "lid_layer";
    lid.kind = "Explicit";
    const std::vector<std::array<Real, 2>> polygon{
        {2, 0}, {54, 0}, {56, 2}, {56, 42}, {54, 44}, {2, 44}, {0, 42}, {0, 2}};
    lid.regions = {{"lid", material, "lid", {stack.outline.box, polygon}}};
    stack.layers.push_back(std::move(ring));
    stack.layers.push_back(std::move(lid));
    for (auto& layer : stack.layers) {
        layer.thickness = 1;
        layer.n_elem_min = 1;
    }

    CutOptions options;
    options.quad_degree = 6;
    options.export_polyhedra = true;
    options.keep_void = false;
    options.snap_phi = k1::threshold("snap_phi");
    options.small_sub_vf = k1::threshold("small_sub_vf");
    const auto volume_tolerance = k1::threshold("cut_volume_relative");
    const auto body_of = [](const HexMeshArrays& mesh, Local attr) {
        expect(attr > 0 && static_cast<std::size_t>(attr) <= mesh.attr_table.entries.size(),
               "lid route: valid attribute ID");
        return mesh.attr_table.entries[attr - 1].body;
    };
    const auto vertex = [](const HexMeshArrays& mesh, std::size_t e, int node) {
        return &mesh.vertices[3 * mesh.hex[8 * e + node]];
    };
    const auto jacobian = [&](const HexMeshArrays& mesh, std::size_t e) {
        const auto* lo = vertex(mesh, e, 0);
        const auto* hi = vertex(mesh, e, 6);
        return (hi[0] - lo[0]) * (hi[1] - lo[1]) * (hi[2] - lo[2]);
    };
    const auto check_subcells = [](const CutLayerArrays& cut) {
        expect(cut.sub_attribute.size() == cut.sub_volfrac.size() &&
                   cut.sub_off.size() == cut.cut_elem.size() + 1 && cut.sub_off.front() == 0 &&
                   static_cast<std::size_t>(cut.sub_off.back()) == cut.sub_volfrac.size() &&
                   std::is_sorted(cut.sub_off.begin(), cut.sub_off.end()),
               "lid route: subcell arrays and offsets");
    };
    const auto build_grid = [&](char name, Local nominal_nx, Local nominal_ny) {
        auto grid_options = k1::options();
        // The fixture specifies whole-outline resolutions. The planner takes a
        // minimum count per feature interval, with 4 the shortest on both axes.
        constexpr Real shortest_interval = 4;
        grid_options.nx =
            std::max(Local{1}, static_cast<Local>(shortest_interval * nominal_nx / (xmax - xmin)));
        grid_options.ny =
            std::max(Local{1}, static_cast<Local>(shortest_interval * nominal_ny / (ymax - ymin)));
        grid_options.levels = 1;
        auto grid = build_tensor_grid(stack, grid_options);
        if (!grid)
            std::cerr << "lid route " << name << " (nominal=" << nominal_nx << 'x' << nominal_ny
                      << ", nx=" << grid_options.nx << ", ny=" << grid_options.ny
                      << ", levels=" << grid_options.levels
                      << ", budget=" << grid_options.cell_budget
                      << ", max_ratio=" << grid_options.max_ratio << "): " << grid.error().path
                      << ": " << grid.error().reason << '\n';
        return grid;
    };
    // Attempt both resolutions so the test reports both grid errors if setup fails.
    const std::array grids{build_grid('A', 20, 16), build_grid('B', 56, 44)};
    for (std::size_t resolution = 0; resolution < grids.size(); ++resolution) {
        const bool coarse = resolution == 0;
        const auto& grid = grids[resolution];
        expect(bool(grid), "lid route: build_tensor_grid succeeds");
        const auto& original = grid.value();
        const auto& bodies = original.attr_table.bodies;
        expect(bodies.contains("substrate") && bodies.contains("ring") && bodies.contains("lid"),
               "lid route: expected body names");
        const auto substrate_id = bodies.at("substrate");
        const auto ring_id = bodies.at("ring");
        const auto lid_id = bodies.at("lid");
        expect(substrate_id != 0 && ring_id != 0 && lid_id != 0 && substrate_id != ring_id &&
                   substrate_id != lid_id && ring_id != lid_id,
               "lid route: distinct nonzero body IDs");
        for (const auto& [name, id] : bodies) {
            auto lower = name;
            std::transform(name.begin(), name.end(), lower.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            expect(lower == name || (lower != "ring" && lower != "lid"),
                   "lid route: exact capitalization for ring and lid");
        }
        auto pending = original.pending_cut;
        expect(!pending.empty(), "lid route: pending cuts exist");
        std::sort(pending.begin(), pending.end());
        for (auto e : pending)
            expect(e >= 0 && static_cast<std::size_t>(e) < original.attribute.size() &&
                       body_of(original, original.attribute[e]) == lid_id,
                   "lid route: pending cut belongs to lid");

        const auto diagonal = [&](std::size_t edge) {
            const auto& a = polygon[edge];
            const auto& b = polygon[(edge + 1) % polygon.size()];
            const auto dx = b[0] - a[0], dy = b[1] - a[1];
            const auto length = std::hypot(dx, dy);
            const std::array<Real, 3> normal{dy / length, -dx / length, 0};
            return Plane{lid_id, normal, normal[0] * a[0] + normal[1] * a[1]};
        };
        const CutGeometry geometry{{{lid_id, {0, -1, 0}, 0},
                                    diagonal(1),
                                    {lid_id, {1, 0, 0}, 56},
                                    diagonal(3),
                                    {lid_id, {0, 1, 0}, 44},
                                    diagonal(5),
                                    {lid_id, {-1, 0, 0}, 0},
                                    diagonal(7)}};

        const auto& x = original.axes[0];
        const auto& y = original.axes[1];
        expect(x.size() >= 2 && x.front() == xmin && x.back() == xmax && y.size() >= 2 &&
                   y.front() == ymin && y.back() == ymax,
               "lid route: axes span the outline");
        const std::array hx{x[1] - x.front(), x.back() - x[x.size() - 2]};
        const std::array hy{y[1] - y.front(), y.back() - y[y.size() - 2]};
        if (coarse)
            expect(hx[0] > 2 && hx[1] > 2 && hy[0] > 2 && hy[1] > 2,
                   "lid route: A corner widths exceed chamfer legs");
        else
            expect(hx[0] == 1 && hx[1] == 1 && hy[0] == 1 && hy[1] == 1,
                   "lid route: B axes have four unit corner boxes");

        std::set<std::pair<Real, Real>> lid_columns;
        std::size_t lid_cells = 0;
        std::vector<Local> expected_cuts;
        for (std::size_t e = 0; e < original.attribute.size(); ++e) {
            if (body_of(original, original.attribute[e]) != lid_id)
                continue;
            ++lid_cells;
            const auto* lo = vertex(original, e, 0);
            const auto* hi = vertex(original, e, 6);
            lid_columns.emplace(lo[0], lo[1]);
            bool crossed = false;
            for (auto x_corner : {xmin, xmax})
                for (auto y_corner : {ymin, ymax}) {
                    // Each diagonal is at Manhattan distance 2 from its outline corner.
                    const auto dx_lo = std::abs(lo[0] - x_corner);
                    const auto dx_hi = std::abs(hi[0] - x_corner);
                    const auto dy_lo = std::abs(lo[1] - y_corner);
                    const auto dy_hi = std::abs(hi[1] - y_corner);
                    const auto near = std::min(dx_lo, dx_hi) + std::min(dy_lo, dy_hi);
                    const auto far = std::max(dx_lo, dx_hi) + std::max(dy_lo, dy_hi);
                    if (!coarse)
                        expect(near != 0, "lid route: B omits unit corner lid cells");
                    crossed = crossed || (near < 2 && 2 < far);
                }
            if (crossed)
                expected_cuts.push_back(static_cast<Local>(e));
        }
        expect(!lid_columns.empty() && lid_cells % lid_columns.size() == 0,
               "lid route: lid cells divide into XY columns");
        const auto nz_lid = lid_cells / lid_columns.size();

        const auto result = build_cut_layer(original, geometry, options);
        expect(bool(result), "lid route: const plane cut succeeds");
        const auto& cut = result.value();
        expect(!cut.cut_elem.empty(), "lid route: cut parents exist");
        auto cut_indices = cut.cut_elem;
        std::sort(cut_indices.begin(), cut_indices.end());
        expect(cut_indices == expected_cuts, "lid route: only diagonal interior crossings cut");
        expect(cut.cut_elem.size() == (coarse ? 4 : 8) * nz_lid, "lid route: cut parent count");
        if (coarse)
            expect(cut.void_elem.empty(), "lid route: A has no void elements");
        check_subcells(cut);

        expect(cut.polyhedra && cut.polyhedra->volume.size() == cut.sub_volfrac.size() &&
                   cut.polyhedra->volfrac_ref.size() == cut.sub_volfrac.size(),
               "lid route: export for every subcell");
        expect(cut.vq_off.size() == cut.sub_volfrac.size() + 1 && cut.vq_off.front() == 0 &&
                   static_cast<std::size_t>(cut.vq_off.back()) == cut.vq_w.size() &&
                   std::is_sorted(cut.vq_off.begin(), cut.vq_off.end()),
               "lid route: volume quadrature offsets");
        const auto& poly = *cut.polyhedra;
        auto physical_poly = poly;
        Real max_quadrature_error = 0;
        Real max_polyhedron_error = 0;
        for (std::size_t i = 0; i < cut.cut_elem.size(); ++i) {
            const auto e = cut.cut_elem[i];
            expect(std::binary_search(pending.begin(), pending.end(), e) &&
                       body_of(original, original.attribute[e]) == lid_id,
                   "lid route: cut parent belongs to lid and was pending");
            const auto begin = cut.sub_off[i], end = cut.sub_off[i + 1];
            expect(coarse ? end - begin == 1 : end > begin, "lid route: subcells per cut parent");
            const auto* lo = vertex(original, e, 0);
            const auto expected = coarse ? 1 - 2 / (hx[lo[0] != xmin] * hy[lo[1] != ymin]) : .5;
            const auto jac = jacobian(original, e);
            for (auto sub = begin; sub < end; ++sub) {
                expect(body_of(original, cut.sub_attribute[sub]) == lid_id,
                       "lid route: cut subcell belongs to lid");
                const auto vf = cut.sub_volfrac[sub];
                expect(std::abs(vf - expected) <= 1e-12, "lid route: chamfer volume fraction");
                const auto weight = std::accumulate(cut.vq_w.begin() + cut.vq_off[sub],
                                                    cut.vq_w.begin() + cut.vq_off[sub + 1],
                                                    0.0);
                expect(std::abs(weight - vf) <= 1e-12 * vf, "lid route: volume quadrature sum");
                const auto volume = jac * vf;
                expect(std::abs(poly.volume[sub] - volume) <= 1e-12 * volume,
                       "lid route: exported volume matches subcell");
                max_quadrature_error = std::max(max_quadrature_error, std::abs(weight - vf) / vf);
                max_polyhedron_error =
                    std::max(max_polyhedron_error, std::abs(poly.volume[sub] - volume) / volume);
                physical_poly.volfrac_ref[sub] *= jac;
            }
        }
        expect(max_quadrature_error <= 1e-12, "lid route: maximum quadrature relative error");
        expect(max_polyhedron_error <= 1e-12, "lid route: maximum polyhedron relative error");
        check_poly(physical_poly, options.snap_phi);
        for (auto sub : cut.small_subs)
            expect(sub >= 0 && static_cast<std::size_t>(sub) < cut.sub_volfrac.size() &&
                       cut.sub_volfrac[sub] < options.small_sub_vf,
                   "lid route: small subcell below threshold");

        const std::set<Local> void_indices(cut.void_elem.begin(), cut.void_elem.end());
        const auto check_volumes = [&](const HexMeshArrays& mesh, const CutLayerArrays& route) {
            const std::set<Local> voids(route.void_elem.begin(), route.void_elem.end());
            std::map<BodyId, long double> volumes;
            for (std::size_t e = 0; e < mesh.attribute.size(); ++e) {
                const auto local = static_cast<Local>(e);
                if (voids.contains(local))
                    continue;
                const auto jac = jacobian(mesh, e);
                const auto parent = std::find(route.cut_elem.begin(), route.cut_elem.end(), local);
                if (parent == route.cut_elem.end()) {
                    volumes[body_of(mesh, mesh.attribute[e])] += jac;
                } else {
                    const auto i = parent - route.cut_elem.begin();
                    for (auto sub = route.sub_off[i]; sub < route.sub_off[i + 1]; ++sub) {
                        const auto body = body_of(mesh, route.sub_attribute[sub]);
                        expect(body == lid_id, "lid route: volume belongs to lid subcell");
                        volumes[body] += jac * route.sub_volfrac[sub];
                    }
                }
            }
            expect(std::abs(volumes[substrate_id] - 2464) <= volume_tolerance * 2464,
                   "lid route: substrate total volume");
            expect(std::abs(volumes[ring_id] - 416) <= volume_tolerance * 416,
                   "lid route: ring total volume");
            expect(std::abs(volumes[lid_id] - 2456) <= volume_tolerance * 2456,
                   "lid route: lid total volume");
            return std::array{std::abs(volumes[substrate_id] - 2464) / 2464,
                              std::abs(volumes[ring_id] - 416) / 416,
                              std::abs(volumes[lid_id] - 2456) / 2456};
        };
        const auto const_errors = check_volumes(original, cut);
        auto compact = original;
        const auto compact_result = build_cut_layer(compact, geometry, options);
        expect(bool(compact_result), "lid route: mutable plane cut succeeds");
        const auto& compact_cut = compact_result.value();
        expect(compact_cut.void_elem.empty() &&
                   compact.attribute.size() + cut.void_elem.size() == original.attribute.size(),
               "lid route: mutable cut removes voids");
        std::vector<chipcae::Index> retained_gids;
        for (std::size_t e = 0; e < original.elem_gid.size(); ++e)
            if (!void_indices.contains(static_cast<Local>(e)))
                retained_gids.push_back(original.elem_gid[e]);
        expect(compact.elem_gid == retained_gids, "lid route: exact retained element GID sequence");
        expect(compact_cut.cut_elem_gid == cut.cut_elem_gid &&
                   compact_cut.cut_elem.size() == compact_cut.cut_elem_gid.size() &&
                   cut.cut_elem.size() == cut.cut_elem_gid.size(),
               "lid route: both overloads preserve cut GIDs");
        for (std::size_t i = 0; i < compact_cut.cut_elem.size(); ++i) {
            const auto e = compact_cut.cut_elem[i];
            expect(e >= 0 && static_cast<std::size_t>(e) < compact.elem_gid.size() &&
                       compact.elem_gid[e] == compact_cut.cut_elem_gid[i] &&
                       original.elem_gid[cut.cut_elem[i]] == cut.cut_elem_gid[i],
                   "lid route: cut indices resolve to parent GIDs");
        }
        check_subcells(compact_cut);
        expect(compact_cut.polyhedra &&
                   compact_cut.polyhedra->volume.size() == compact_cut.sub_volfrac.size() &&
                   compact_cut.polyhedra->volfrac_ref.size() == compact_cut.sub_volfrac.size(),
               "lid route: mutable export for every subcell");
        auto compact_physical_poly = *compact_cut.polyhedra;
        for (std::size_t i = 0; i < compact_cut.cut_elem.size(); ++i) {
            const auto jac = jacobian(compact, compact_cut.cut_elem[i]);
            for (auto sub = compact_cut.sub_off[i]; sub < compact_cut.sub_off[i + 1]; ++sub)
                compact_physical_poly.volfrac_ref[sub] *= jac;
        }
        check_poly(compact_physical_poly, options.snap_phi);
        const auto mutable_errors = check_volumes(compact, compact_cut);

        const auto replay = build_cut_layer(original, geometry, options);
        expect(bool(replay), "lid route: const replay succeeds");
        const auto same_bits = [](const auto& a, const auto& b, const char* message) {
            expect(a.size() == b.size(), message);
            for (std::size_t i = 0; i < a.size(); ++i)
                expect(std::bit_cast<std::uint64_t>(a[i]) == std::bit_cast<std::uint64_t>(b[i]),
                       message);
        };
        same_bits(replay.value().vq_w, cut.vq_w, "lid route: replay quadrature weight bits");
        same_bits(replay.value().sub_volfrac, cut.sub_volfrac, "lid route: replay fraction bits");

        auto reverse_geometry = geometry;
        std::reverse(reverse_geometry.planes.begin(), reverse_geometry.planes.end());
        const auto reversed = build_cut_layer(original, reverse_geometry, options);
        expect(bool(reversed), "lid route: reversed plane cut succeeds");
        const auto& reverse_cut = reversed.value();
        auto reverse_indices = reverse_cut.cut_elem;
        std::sort(reverse_indices.begin(), reverse_indices.end());
        expect(reverse_indices == cut_indices, "lid route: reversed planes cut the same parents");
        check_subcells(reverse_cut);
        for (std::size_t i = 0; i < cut.cut_elem.size(); ++i) {
            const auto it = std::find(
                reverse_cut.cut_elem.begin(), reverse_cut.cut_elem.end(), cut.cut_elem[i]);
            const auto j = it - reverse_cut.cut_elem.begin();
            const auto count = cut.sub_off[i + 1] - cut.sub_off[i];
            expect(count == reverse_cut.sub_off[j + 1] - reverse_cut.sub_off[j],
                   "lid route: reversed planes preserve parent subcell count");
            for (Local offset = 0; offset < count; ++offset)
                expect(std::abs(cut.sub_volfrac[cut.sub_off[i] + offset] -
                                reverse_cut.sub_volfrac[reverse_cut.sub_off[j] + offset]) <= 1e-15,
                       "lid route: reversed planes preserve subcell fractions");
        }
        std::ostringstream diagnostics;
        diagnostics << std::setprecision(17) << "lid route " << (coarse ? 'A' : 'B') << " 实测"
                    << " xy_cells=" << x.size() - 1 << 'x' << y.size() - 1
                    << " lid_slabs=" << nz_lid << " cut_subcells=" << cut.sub_volfrac.size()
                    << " const_body_errors=" << const_errors[0] << ',' << const_errors[1] << ','
                    << const_errors[2] << " mutable_body_errors=" << mutable_errors[0] << ','
                    << mutable_errors[1] << ',' << mutable_errors[2]
                    << " max_quadrature_error=" << max_quadrature_error
                    << " max_polyhedron_error=" << max_polyhedron_error << '\n';
        std::clog << diagnostics.str();
    }
}

// Contract comparisons include every array, preserve order and compare floating-point bits.
template <class T>
void same_bits(const std::vector<T>& a, const std::vector<T>& b) {
    expect(a.size() == b.size(), "array size differs");
    if (!a.empty())
        expect(std::memcmp(a.data(), b.data(), a.size() * sizeof(T)) == 0, "array bits differ");
}
template <class T, std::size_t N>
void same_bits(const std::array<std::vector<T>, N>& a, const std::array<std::vector<T>, N>& b) {
    for (std::size_t i = 0; i < N; ++i)
        same_bits(a[i], b[i]);
}
void same_mesh(const HexMeshArrays& a, const HexMeshArrays& b) {
    same_bits(a.vertices, b.vertices);
    same_bits(a.hex, b.hex);
    same_bits(a.attribute, b.attribute);
    same_bits(a.bdr_quad, b.bdr_quad);
    same_bits(a.bdr_attribute, b.bdr_attribute);
    same_bits(a.elem_gid, b.elem_gid);
    same_bits(a.node_gid, b.node_gid);
    same_bits(a.partitioning, b.partitioning);
    same_bits(a.node_layer, b.node_layer);
    same_bits(a.copper_layer_of_layer, b.copper_layer_of_layer);
    same_bits(a.copper_phi, b.copper_phi);
    same_bits(a.node_column, b.node_column);
    same_bits(a.attr_frame, b.attr_frame);
    same_bits(a.nodes, b.nodes);
    same_bits(a.pending_cut, b.pending_cut);
    same_bits(a.axes, b.axes);
    expect(a.order == b.order && a.internal_interfaces == b.internal_interfaces,
           "mesh scalar metadata differs");
    expect(a.snapped.size() == b.snapped.size(), "snap report size");
    for (std::size_t i = 0; i < a.snapped.size(); ++i) {
        expect(a.snapped[i].axis == b.snapped[i].axis, "snap axis");
        same_bits(
            std::vector<double>{a.snapped[i].from, a.snapped[i].to, a.snapped[i].displacement},
            std::vector<double>{b.snapped[i].from, b.snapped[i].to, b.snapped[i].displacement});
    }
    expect(a.preparation_report.size() == b.preparation_report.size(), "preparation report size");
    for (std::size_t i = 0; i < a.preparation_report.size(); ++i) {
        const auto& x = a.preparation_report[i];
        const auto& y = b.preparation_report[i];
        expect(x.name == y.name && x.side_a_bodies == y.side_a_bodies &&
                   x.side_b_bodies == y.side_b_bodies && x.aligned_faces == y.aligned_faces &&
                   x.cut_faces == y.cut_faces,
               "preparation metadata");
        same_bits(std::vector<double>{x.aligned_area, x.cut_area},
                  std::vector<double>{y.aligned_area, y.cut_area});
    }
    const auto& x = a.grid_report;
    const auto& y = b.grid_report;
    expect(x.levels == y.levels && x.rule == y.rule && x.N_L == y.N_L &&
               x.auto_selected == y.auto_selected && x.padding == y.padding &&
               x.warnings == y.warnings && x.void_cells == y.void_cells,
           "grid metadata");
    same_bits(std::vector<double>{x.partition_imbalance},
              std::vector<double>{y.partition_imbalance});
    same_bits(x.max_aspect_ratio, y.max_aspect_ratio);
    expect(x.level_axes.size() == y.level_axes.size() &&
               x.column_ranks.size() == y.column_ranks.size(),
           "hierarchy sizes");
    for (std::size_t i = 0; i < x.level_axes.size(); ++i)
        same_bits(x.level_axes[i], y.level_axes[i]);
    for (std::size_t i = 0; i < x.column_ranks.size(); ++i)
        same_bits(x.column_ranks[i], y.column_ranks[i]);
    expect(a.face_sets == b.face_sets && a.attr_table.bodies == b.attr_table.bodies &&
               a.attr_table.layers == b.attr_table.layers &&
               a.attr_table.materials == b.attr_table.materials &&
               a.attr_table.references == b.attr_table.references &&
               a.attr_table.boundary_names == b.attr_table.boundary_names,
           "mesh metadata differs");
    expect(a.attr_table.entries.size() == b.attr_table.entries.size(), "attribute table size");
    for (std::size_t i = 0; i < a.attr_table.entries.size(); ++i) {
        const auto& x = a.attr_table.entries[i];
        const auto& y = b.attr_table.entries[i];
        expect(x.layer == y.layer && x.region == y.region && x.material == y.material &&
                   x.body == y.body,
               "attribute record differs");
    }
}
void same_cut(const CutLayerArrays& a, const CutLayerArrays& b) {
    same_bits(a.cut_elem, b.cut_elem);
    same_bits(a.sub_off, b.sub_off);
    same_bits(a.sub_attribute, b.sub_attribute);
    same_bits(a.sub_volfrac, b.sub_volfrac);
    same_bits(a.vq_off, b.vq_off);
    same_bits(a.bq_off, b.bq_off);
    same_bits(a.vq_xi, b.vq_xi);
    same_bits(a.bq_xi, b.bq_xi);
    same_bits(a.bq_n, b.bq_n);
    same_bits(a.vq_w, b.vq_w);
    same_bits(a.bq_w, b.bq_w);
    same_bits(a.bq_bdr_attr, b.bq_bdr_attr);
    same_bits(a.gp_elem_gid, b.gp_elem_gid);
    same_bits(a.gp_face, b.gp_face);
    same_bits(a.gp_face_gid, b.gp_face_gid);
    same_bits(a.gp_face_minus, b.gp_face_minus);
    same_bits(a.gp_face_plus, b.gp_face_plus);
    same_bits(a.gp_owner_rank, b.gp_owner_rank);
    same_bits(a.eb_points, b.eb_points);
    same_bits(a.eb_xi, b.eb_xi);
    same_bits(a.eb_normals, b.eb_normals);
    same_bits(a.eb_parent_elem, b.eb_parent_elem);
    same_bits(a.sq_off, b.sq_off);
    same_bits(a.sq_side, b.sq_side);
    same_bits(a.sq_xi, b.sq_xi);
    same_bits(a.sq_w, b.sq_w);
    same_bits(a.cut_elem_gid, b.cut_elem_gid);
    same_bits(a.void_elem, b.void_elem);
    same_bits(a.small_subs, b.small_subs);
    same_bits(a.exterior_area_vector, b.exterior_area_vector);
    expect(a.face_sets == b.face_sets && a.void_attribute == b.void_attribute,
           "cut metadata differs");
    expect(a.level_sets.size() == b.level_sets.size(), "level set count");
    for (std::size_t i = 0; i < a.level_sets.size(); ++i) {
        expect(a.level_sets[i].body == b.level_sets[i].body, "level set body");
        same_bits(a.level_sets[i].phi, b.level_sets[i].phi);
    }
    expect(a.polyhedra.has_value() == b.polyhedra.has_value(), "polyhedra presence");
    if (a.polyhedra) {
        same_bits(a.polyhedra->points, b.polyhedra->points);
        same_bits(a.polyhedra->point_xi, b.polyhedra->point_xi);
        same_bits(a.polyhedra->point_parent_elem, b.polyhedra->point_parent_elem);
        same_bits(a.polyhedra->face_conn, b.polyhedra->face_conn);
        same_bits(a.polyhedra->face_off, b.polyhedra->face_off);
        same_bits(a.polyhedra->poly_faces, b.polyhedra->poly_faces);
        same_bits(a.polyhedra->poly_off, b.polyhedra->poly_off);
        same_bits(a.polyhedra->parent_elem_gid, b.polyhedra->parent_elem_gid);
        same_bits(a.polyhedra->body, b.polyhedra->body);
        same_bits(a.polyhedra->layer_id, b.polyhedra->layer_id);
        same_bits(a.polyhedra->material_id, b.polyhedra->material_id);
        same_bits(a.polyhedra->cell_kind, b.polyhedra->cell_kind);
        same_bits(a.polyhedra->volume, b.polyhedra->volume);
        same_bits(a.polyhedra->volfrac_ref, b.polyhedra->volfrac_ref);
    }
}
HexMeshArrays outline_mesh() {
    auto s = k1::stack();
    auto opt = k1::options();
    opt.nx = 8;
    opt.ny = 8;
    opt.levels = 1;
    return build_tensor_grid(s, opt).value();
}
Footprint chamfer_outline() {
    return {{0, 0, 1, 1},
            {{.25, 0}, {.75, 0}, {1, .25}, {1, .75}, {.75, 1}, {.25, 1}, {0, .75}, {0, .25}}};
}
void outline_cut_api() {
    const auto original = outline_mesh();
    auto mesh = original;
    Footprint empty{{0, 0, 1, 1}, {}};
    auto none = apply_outline_cut(mesh, empty, {});
    expect(bool(none) && !none.value(), "empty outline must return nullopt");
    same_mesh(mesh, original);
    auto outline = chamfer_outline();
    auto reference_mesh = original;
    auto reference = apply_outline_cut(reference_mesh, outline, {});
    expect(bool(reference) && reference.value().has_value(), "outline clipping API missing");
    expect(!reference.value()->cut_elem.empty(), "chamfer must cut cells");
    std::reverse(outline.polygon.begin(), outline.polygon.end());
    auto reversed_mesh = original;
    auto reversed = apply_outline_cut(reversed_mesh, outline, {});
    expect(bool(reversed) && reversed.value().has_value(), "clockwise clipping failed");
    same_mesh(reference_mesh, reversed_mesh);
    same_cut(*reference.value(), *reversed.value());
    outline = chamfer_outline();
    outline.polygon.push_back(outline.polygon.front());
    auto closed_mesh = original;
    auto closed = apply_outline_cut(closed_mesh, outline, {});
    expect(bool(closed) && closed.value().has_value(), "closed clipping failed");
    same_mesh(reference_mesh, closed_mesh);
    same_cut(*reference.value(), *closed.value());
    outline.polygon[0][0] = -0.1;
    auto invalid_mesh = original;
    auto invalid = apply_outline_cut(invalid_mesh, outline, {});
    expect(!invalid && invalid.error().path == "outline", "out-of-box error category");
}
void outline_cut_extra_planes() {
    const auto original = outline_mesh();
    Footprint empty{{0, 0, 1, 1}, {}};
    auto mesh = original;
    auto none = apply_outline_cut(mesh, empty, {}, {});
    expect(bool(none) && !none.value(), "empty extra must return nullopt");
    same_mesh(mesh, original);
    const auto body = original.attr_table.bodies.at("cap");
    CutGeometry extra{{{body, {1, 0, 0}, .7},
                       {body, {-1, 0, 0}, -.3},
                       {body, {0, 1, 0}, .7},
                       {body, {0, -1, 0}, -.3}}};
    CutOptions options;
    options.export_polyhedra = true;
    options.keep_void = false;
    auto direct_mesh = original;
    auto direct = build_cut_layer(direct_mesh, extra, options);
    expect(bool(direct) && !direct.value().cut_elem.empty(), "direct extra fixture failed");
    auto result = apply_outline_cut(mesh, empty, {}, extra);
    expect(bool(result) && result.value().has_value(), "extra-only clipping API missing");
    same_mesh(mesh, direct_mesh);
    same_cut(*result.value(), direct.value());
    // Other bodies retain all original GIDs and attributes despite the target-body cut.
    std::vector<chipcae::Index> before, after;
    for (std::size_t e = 0; e < original.attribute.size(); ++e)
        if (original.attr_table.entries.at(original.attribute[e] - 1).body != body)
            before.push_back(original.elem_gid[e]);
    for (std::size_t e = 0; e < mesh.attribute.size(); ++e)
        if (mesh.attr_table.entries.at(mesh.attribute[e] - 1).body != body)
            after.push_back(mesh.elem_gid[e]);
    expect(!before.empty(), "fixture must exercise another body");
    same_bits(before, after);

    auto outline = chamfer_outline();
    CutGeometry combined;
    // Analytic half spaces of the octagon, in edge order, then body-map order.
    const double r = .25 / std::hypot(.25, .25);
    const std::array<std::array<double, 3>, 8> planes{{{0, -1, 0},
                                                       {r, -r, .75 * r},
                                                       {1, 0, 1},
                                                       {r, r, r + .75 * r},
                                                       {0, 1, 1},
                                                       {-r, r, r - .25 * r},
                                                       {-1, 0, 0},
                                                       {-r, -r, -.25 * r}}};
    for (const auto& p : planes)
        for (const auto& [name, id] : original.attr_table.bodies) {
            (void)name;
            combined.planes.push_back({id, {p[0], p[1], 0}, p[2]});
        }
    combined.planes.insert(combined.planes.end(), extra.planes.begin(), extra.planes.end());
    direct_mesh = original;
    direct = build_cut_layer(direct_mesh, combined, options);
    expect(bool(direct), "combined direct fixture failed");
    mesh = original;
    result = apply_outline_cut(mesh, outline, {}, extra);
    expect(bool(result) && result.value().has_value(), "combined clipping failed");
    same_mesh(mesh, direct_mesh);
    same_cut(*result.value(), direct.value());
    mesh = original;
    auto other_mesh = original;
    result = apply_outline_cut(mesh, outline, {});
    auto other = apply_outline_cut(other_mesh, outline, {}, {});
    expect(bool(result) && result.value() && bool(other) && other.value(), "overload clipping");
    same_mesh(mesh, other_mesh);
    same_cut(*result.value(), *other.value());
}

int main(int argc, char** argv) {
#ifdef CHIPCAE_ENABLE_KOKKOS
    Kokkos::ScopeGuard guard(argc, argv);
#else
    (void)argc;
    (void)argv;
#endif
    if (argc == 2) {
        if (std::string(argv[1]) == "outline_cut_api")
            outline_cut_api();
        else if (std::string(argv[1]) == "outline_cut_extra_planes")
            outline_cut_extra_planes();
        else
            return 2;
        return 0;
    }
    auto s = k1::stack();
    s.activation_groups.clear();
    s.layers.resize(1);
    s.layers[0].thickness = 1;
    s.layers[0].n_elem_min = 1;
    s.layers[0].regions.clear();
    auto o = k1::options();
    auto grid = build_tensor_grid(s, o);
    expect(bool(grid), "unit grid");
    const auto original = grid.value();
    CutOptions options;
    options.export_polyhedra = true;
    options.snap_phi = k1::threshold("snap_phi");
    options.small_sub_vf = k1::threshold("small_sub_vf");
    {
        auto r = build_cut_layer(original, {{{0, {1, 1, 0}, 1}}}, options);
        expect(bool(r), "45 degree cut");
        auto& p = *r.value().polyhedra;
        expect(p.points.size() == 18 && p.face_off.size() == 6,
               "triangular prism: six vertices five faces");
        expect(std::abs(p.volume[0] - .5) / .5 <= 1e-12, "analytic prism volume");
        check_poly(p, options.snap_phi);
        for (const auto& plane :
             std::vector<Plane>{{0, {1, 1, 1}, 1e-13}, {0, {1, 0, 0}, 1}, {0, {1, 1, 0}, 0}}) {
            auto edge = build_cut_layer(original, {{plane}}, options);
            expect(bool(edge), "grazing cut");
            check_poly(*edge.value().polyhedra, options.snap_phi);
        }
    }
    {
        // Make the bottom ring have zero signed area with distinct vertices;
        // discarding it leaves the clipped shell open.
        auto broken = original;
        broken.vertices[3 * broken.hex[3]] = 2;
        broken.vertices[3 * broken.hex[3] + 1] = 1;
        auto rejected = build_cut_layer(broken, {{{0, {0, 0, 1}, .5}}}, options);
        expect(!rejected && rejected.error().path == "cut/polyhedron",
               "open polyhedron rejected during generation");
        const auto& reason = rejected.error().reason;
        expect(reason.find("elem_gid=" + std::to_string(original.elem_gid[0])) !=
                       std::string::npos &&
                   reason.find("rings=") != std::string::npos &&
                   reason.find("signed_volume=") != std::string::npos &&
                   reason.find("closed=false") != std::string::npos,
               "polyhedron error identifies cell and topology diagnostics");
    }
    {
        auto split = o;
        split.nx = 4;
        split.ny = 4;
        const auto mesh = build_tensor_grid(s, split).value();
        auto high = options;
        high.quad_degree = 6;
        auto result = build_cut_layer(mesh, {{{0, {1, 1, .3}, 1.1}}}, high);
        expect(bool(result), "degree six with polyhedron export");
        const auto& c = result.value();
        const auto& p = *c.polyhedra;
        expect(!c.sub_volfrac.empty() && p.volume.size() == c.sub_volfrac.size(),
               "degree six exports every subcell");
        for (std::size_t sub = 0; sub < c.sub_volfrac.size(); ++sub) {
            auto e = c.cut_elem[sub];
            double jac = 1;
            for (int d = 0; d < 3; ++d)
                jac *= mesh.vertices[3 * mesh.hex[8 * e + 6] + d] -
                       mesh.vertices[3 * mesh.hex[8 * e] + d];
            double weight = std::accumulate(
                c.vq_w.begin() + c.vq_off[sub], c.vq_w.begin() + c.vq_off[sub + 1], 0.0);
            expect(p.parent_elem_gid[sub] == c.cut_elem_gid[sub],
                   "degree six matching polyhedron parent");
            expect(std::abs(p.volume[sub] - jac * weight) <= 1e-12 * jac * weight,
                   "degree six polyhedron/quadrature relative agreement");
            expect(std::abs(p.volume[sub] - jac * c.sub_volfrac[sub]) <=
                       1e-12 * jac * c.sub_volfrac[sub],
                   "degree six polyhedron/subcell relative agreement");
        }
    }
    std::mt19937_64 rng(20261116);
    std::uniform_real_distribution<double> uniform(.2, 1);
    std::size_t cut_count = 0;
    double max_conservation_error = 0, max_fraction_error = 0;
    for (int trial = 0; trial < 1006; ++trial) {
        std::array<double, 3> n{uniform(rng), uniform(rng), uniform(rng)};
        double d = (n[0] + n[1] + n[2]) * uniform(rng);
        if (trial >= 1000) {
            n = {1, 1, 1};
            d = trial == 1000   ? 0
                : trial == 1001 ? 1
                : trial == 1002 ? 2
                : trial == 1003 ? 3
                : trial == 1004 ? std::sqrt(options.small_sub_vf)
                                : 1;
        }
        if (trial == 1005)
            n = {1, 0, 0};
        auto plane_n = n;
        auto plane_d = d;
        if (trial < 200)
            for (int axis = 0; axis < 3; ++axis)
                if (rng() & 1) {
                    plane_n[axis] = -n[axis];
                    plane_d -= n[axis];
                }
        // Reflect selected cube axes to exercise all normal octants while
        // retaining the independent positive-weight analytic CDF.
        CutGeometry g{{{0, plane_n, plane_d}}};
        auto r = build_cut_layer(original, g, options);
        expect(bool(r), "plane cut");
        const auto& c = r.value();
        check_poly(*c.polyhedra, options.snap_phi);
        double expected = trial == 1005 ? 1 : static_cast<double>(exact(n, d));
        double v = c.void_elem.empty() ? 1 : 0;
        if (!c.cut_elem.empty()) {
            ++cut_count;
            v = std::accumulate(c.vq_w.begin(), c.vq_w.end(), 0.0);
            double scale = expected;
            expect(std::abs(v - expected) <= k1::threshold("cut_volume_relative") * scale,
                   "G1 analytic volume");
            auto closure = c.exterior_area_vector[0];
            for (std::size_t q = 0; q < c.bq_w.size(); ++q)
                for (int k = 0; k < 3; ++k)
                    closure[k] += c.bq_n[k][q] * c.bq_w[q];
            expect(std::sqrt(
                       std::inner_product(closure.begin(), closure.end(), closure.begin(), 0.0)) <=
                       6 * k1::threshold("closure_relative"),
                   "G1 closure norm");
            if (trial == 1004)
                expect(!c.small_subs.empty() && v > 0, "small sliver retained and marked");
            for (const auto& x : c.vq_xi)
                for (auto p : x)
                    expect(p >= 0 && p <= 1, "reference coordinates");
            double moment = 0;
            for (std::size_t q = 0; q < c.vq_w.size(); ++q)
                moment += c.vq_w[q] * c.vq_xi[0][q] * c.vq_xi[0][q];
            expect(moment >= 0 && moment <= v, "positive degree-two rule");
            expect(std::is_sorted(c.vq_off.begin(), c.vq_off.end()) &&
                       std::is_sorted(c.bq_off.begin(), c.bq_off.end()),
                   "CSR monotone");
        } else
            expect(std::abs(v - expected) <= k1::threshold("cut_volume_relative"),
                   "degenerate volume");
        if (trial < 200) {
            max_conservation_error = std::max(max_conservation_error, std::abs(v - expected));
            expect(std::abs(v - expected) <= 1e-12,
                   "200 random conservation against analytic void");
            const auto& poly = *c.polyhedra;
            expect(poly.volume.size() == c.sub_volfrac.size(), "every random cut exported");
            for (std::size_t i = 0; i < poly.volume.size(); ++i) {
                max_fraction_error =
                    std::max(max_fraction_error, std::abs(poly.volume[i] - c.sub_volfrac[i]));
                expect(std::abs(poly.volume[i] - c.sub_volfrac[i]) <= 1e-12,
                       "random fraction agreement");
            }
            auto disabled = options;
            disabled.export_polyhedra = false;
            auto legacy = build_cut_layer(original, g, disabled);
            expect(bool(legacy) && !legacy.value().polyhedra &&
                       legacy.value().sub_volfrac == c.sub_volfrac &&
                       legacy.value().vq_w == c.vq_w && legacy.value().bq_w == c.bq_w &&
                       legacy.value().vq_xi == c.vq_xi && legacy.value().bq_xi == c.bq_xi,
                   "export does not change existing numerics");
        }
        auto again = build_cut_layer(original, g, options);
        expect(again.value().vq_w == c.vq_w && again.value().bq_w == c.bq_w, "cut replay");
    }
    {
        auto stack = k1::stack();
        auto opts = k1::options();
        auto source = build_tensor_grid(stack, opts).value();
        auto layered = source;
        auto cut = build_cut_layer(layered, {{{0, {1, 1, 0}, .8}}}, options);
        expect(bool(cut), "layered interface cut");
        for (auto attr : cut.value().bq_bdr_attr) {
            expect(attr > 6, "dedicated EB attribute");
            bool registered = false;
            for (const auto& [name, list] : layered.face_sets)
                if (name.starts_with("interface:") && name.ends_with("|void") &&
                    std::find(list.begin(), list.end(), attr) != list.end())
                    registered = true;
            expect(registered, "registered EB face group");
        }
        expect(!layered.face_sets.contains("exposed:void"), "no exposed:void group");
        chipcae::Index aligned = 0;
        bool listed = false;
        for (const auto& r : layered.preparation_report)
            if (r.name.ends_with("|void")) {
                aligned += r.aligned_faces;
                listed = listed || !r.side_a_bodies.empty();
            }
        expect(aligned > 0, "aligned exposed faces counted");
        expect(listed, "void interface lists the solid side");
        for (std::size_t n = 0; n < layered.node_gid.size(); ++n) {
            auto it =
                std::find(source.node_gid.begin(), source.node_gid.end(), layered.node_gid[n]);
            expect(it != source.node_gid.end(), "original node GID retained");
            auto old = static_cast<std::size_t>(it - source.node_gid.begin());
            for (int d = 0; d < 3; ++d)
                expect(layered.vertices[3 * n + d] == source.vertices[3 * old + d],
                       "node GID geometry retained");
        }
        auto eb = resolve_cut_activation(stack, layered, cut.value(), {"interface:body:base|void"});
        auto reversed =
            resolve_cut_activation(stack, layered, cut.value(), {"interface:void|body:base"});
        expect(bool(eb) && bool(reversed) && !eb.value().faces.eb_subs.empty() &&
                   eb.value().faces.bdr_attributes == reversed.value().faces.bdr_attributes,
               "canonical embedded interface");
    }
    auto split = o;
    split.nx = 4;
    split.ny = 4;
    auto mesh = build_tensor_grid(s, split).value();
    auto r = build_cut_layer(mesh, {{{0, {1, 1, 0}, 1}}}, options);
    expect(bool(r), "pruned cut");
    auto& c = r.value();
    expect(mesh.attribute.size() < 16, "void cells removed");
    const auto& poly = *c.polyhedra;
    for (std::size_t v = 0; v < poly.point_parent_elem.size(); ++v) {
        auto e = poly.point_parent_elem[v];
        expect(e >= 0 && static_cast<std::size_t>(e) < mesh.attribute.size(),
               "remapped parent local ID");
        for (int d = 0; d < 3; ++d) {
            auto lo = mesh.vertices[3 * mesh.hex[8 * e] + d];
            auto hi = mesh.vertices[3 * mesh.hex[8 * e + 6] + d];
            expect(std::abs(lo + poly.point_xi[3 * v + d] * (hi - lo) - poly.points[3 * v + d]) <=
                       1e-14,
                   "reference interpolation location survives compaction");
        }
    }
    for (std::size_t i = 0; i < poly.volume.size(); ++i) {
        expect(std::abs(poly.volume[i] / (.25 * .25) - c.sub_volfrac[i]) <= 1e-12,
               "nonunit parent volume fraction");
        expect(poly.parent_elem_gid[i] == c.cut_elem_gid[i], "parent GID survives compaction");
    }
    std::set<Local> attrs(mesh.attribute.begin(), mesh.attribute.end());
    expect(*attrs.begin() == 1, "compact after void removal");
    auto cache = GhostPenaltyCache::prepare(mesh, c, {{1}, {}});
    expect(bool(cache), "cache prepare");
    auto all = cache.value().lookup({1});
    auto expected_hash = mask_hash(activated_gids(mesh, {1}));
    expect(std::find(cache.value().hashes.begin(), cache.value().hashes.end(), expected_hash) !=
               cache.value().hashes.end(),
           "cache hashes activated element GIDs");
    expect(bool(all) && bool(cache.value().lookup({})), "two mask hits");
    expect(!cache.value().lookup({999}), "unbudgeted mask");
    std::size_t brute = 0;
    std::set<Local> cuts(c.cut_elem.begin(), c.cut_elem.end());
    for (std::size_t a = 0; a < mesh.attribute.size(); ++a)
        for (std::size_t b = a + 1; b < mesh.attribute.size(); ++b) {
            int common = 0;
            for (int i = 0; i < 8; ++i)
                for (int j = 0; j < 8; ++j)
                    common += mesh.hex[8 * a + i] == mesh.hex[8 * b + j];
            if (common == 4 &&
                (cuts.contains(static_cast<Local>(a)) || cuts.contains(static_cast<Local>(b))))
                ++brute;
        }
    expect(all.value().face.size() == brute, "GP brute enumeration");
    std::vector<bool> used(mesh.node_gid.size());
    for (auto v : mesh.hex)
        used[v] = true;
    expect(std::all_of(used.begin(), used.end(), [](bool v) { return v; }),
           "no unreferenced vertices");
    for (auto gid : mesh.node_gid)
        expect(std::find(original.node_gid.begin(), original.node_gid.end(), gid) !=
                       original.node_gid.end() ||
                   gid >= 0,
               "retained node GID");
    for (std::size_t i = 0; i < all.value().face_gid.size(); ++i) {
        expect(all.value().elem_gid[0][i] < all.value().elem_gid[1][i], "GP ordered GIDs");
        expect(all.value().face_minus[i] >= 0 && all.value().face_minus[i] < 6 &&
                   all.value().face_plus[i] >= 0 && all.value().face_plus[i] < 6,
               "MFEM local face numbers");
    }
    expect(mask_hash({1, 2, 1}) == mask_hash({2, 1}) && mask_hash({1, 2}) != mask_hash({1, 3}),
           "hash unique GID set");
    auto face_device = to_device<chipcae::HostSpace>(all.value());
    expect(face_device.face_gid.extent(0) == all.value().face.size(), "GP flat views");
    auto device = to_device<chipcae::HostSpace>(c);
    expect(device.ls_phi.extent(0) == c.level_sets.size() * mesh.node_gid.size(),
           "flat level sets");
    expect(device.vq_w.size() == c.vq_w.size(), "A1 device mirror");
    auto activation = resolve_cut_activation(s, mesh, c, {"layer:base"});
    expect(bool(activation) && activation.value().cells.cut_subs.size() == c.cut_elem.size(),
           "cut activation");
    auto fillet = build_cut_layer(original, {{{0, {1, 0, 0}, .8}, {0, {0, 1, 0}, .7}}}, options);
    expect(bool(fillet), "piecewise planar fillet");
    check_poly(*fillet.value().polyhedra, options.snap_phi);
    expect(std::abs(fillet.value().sub_volfrac[0] - .56) <= k1::threshold("cut_volume_relative"),
           "multi-plane intersection");
    auto tetra = build_cut_layer(original, {{{0, {1, 1, 1}, 1}}}, options);
    double quadratic = 0;
    for (std::size_t q = 0; q < tetra.value().vq_w.size(); ++q)
        quadratic += tetra.value().vq_w[q] * std::pow(tetra.value().vq_xi[0][q], 2);
    expect(std::abs(quadratic - 1.0 / 60) <= k1::threshold("cut_volume_relative") / 60,
           "degree two tetra exactness");
    auto factorial = [](int n) {
        double x = 1;
        for (int i = 2; i <= n; ++i)
            x *= i;
        return x;
    };
    for (int degree : {2, 4, 6, 8}) {
        auto high = options;
        high.quad_degree = degree;
        auto cut = build_cut_layer(original, {{{0, {1, 1, 1}, 1}}}, high);
        expect(bool(cut), "higher degree cut");
        for (int a = 0; a <= degree; ++a)
            for (int b = 0; b + a <= degree; ++b)
                for (int d = 0; d + a + b <= degree; ++d) {
                    double vol = 0, surf = 0;
                    for (std::size_t q = 0; q < cut.value().vq_w.size(); ++q)
                        vol += cut.value().vq_w[q] * std::pow(cut.value().vq_xi[0][q], a) *
                               std::pow(cut.value().vq_xi[1][q], b) *
                               std::pow(cut.value().vq_xi[2][q], d);
                    for (std::size_t q = 0; q < cut.value().bq_w.size(); ++q)
                        surf += cut.value().bq_w[q] * std::pow(cut.value().bq_xi[0][q], a) *
                                std::pow(cut.value().bq_xi[1][q], b) *
                                std::pow(cut.value().bq_xi[2][q], d);
                    double ev =
                        factorial(a) * factorial(b) * factorial(d) / factorial(a + b + d + 3);
                    double es = std::sqrt(3.) * factorial(a) * factorial(b) * factorial(d) /
                                factorial(a + b + d + 2);
                    expect(std::abs(vol - ev) <= 1e-12 * ev, "volume polynomial exactness");
                    expect(std::abs(surf - es) <= 1e-12 * es, "interface polynomial exactness");
                }
    }
    auto default_cut = build_cut_layer(original, {{{0, {1, 1, 1}, 1}}}, CutOptions{});
    // (1,2,3) yields pentagons away from d=3; its largest component equals
    // the sum of the others, so use (1,2,2.5) as well for a true hexagon.
    for (auto normal : {std::array<double, 3>{1, 2, 3}, std::array<double, 3>{1, 2, 2.5}}) {
        double offset = normal[2] == 3 ? 2.5 : 2.75;
        int triangles = normal[2] == 3 ? 3 : 4;
        for (int degree : {2, 4, 6, 8}) {
            auto opts = options;
            opts.quad_degree = degree;
            std::array<CutLayerArrays, 2> sides;
            std::array<double, 2> areas{};
            for (int side = 0; side < 2; ++side) {
                double sign = side == 0 ? 1 : -1;
                auto sided_normal = normal;
                for (auto& x : sided_normal)
                    x *= sign;
                auto mesh_copy = original;
                auto result =
                    build_cut_layer(mesh_copy, {{{0, sided_normal, sign * offset}}}, opts);
                expect(bool(result), "oblique cut");
                sides[side] = result.value();
                const auto& c = sides[side];
                int n = degree / 2 + 1;
                int nv = degree == 2 ? 4 : n * n * n;
                int nb = degree == 2 ? 3 : n * n;
                int tets = normal[2] == 3 && side == 0 ? 12 : 16;
                expect(c.vq_w.size() == std::size_t(tets * nv), "point count per fan tetrahedron");
                expect(c.bq_w.size() == std::size_t(triangles * nb),
                       "point count per interface triangle");
                for (auto w : c.vq_w)
                    expect(w > 0, "positive volume weights");
                for (auto w : c.bq_w)
                    expect(w > 0, "positive boundary weights");
                for (const auto& x : c.vq_xi)
                    for (auto v : x)
                        expect(v > 0 && v < 1, "strictly interior volume points");
                for (const auto& x : c.bq_xi)
                    for (auto v : x)
                        expect(v > 0 && v < 1, "interface points inside cube");
                for (std::size_t sub = 0; sub < c.sub_volfrac.size(); ++sub) {
                    double volume = std::accumulate(
                        c.vq_w.begin() + c.vq_off[sub], c.vq_w.begin() + c.vq_off[sub + 1], 0.0);
                    expect(std::abs(volume - c.sub_volfrac[sub]) <= 1e-12 * c.sub_volfrac[sub],
                           "subcell weight/volume agreement");
                    double area = std::accumulate(
                        c.bq_w.begin() + c.bq_off[sub], c.bq_w.begin() + c.bq_off[sub + 1], 0.0);
                    const auto& av = c.exterior_area_vector[sub];
                    double geometric_area =
                        std::sqrt(std::inner_product(av.begin(), av.end(), av.begin(), 0.0));
                    expect(std::abs(area - geometric_area) <= 1e-12 * geometric_area,
                           "subcell weight/geometric interface area agreement");
                    areas[side] += area;
                }
                double reported_area = 0;
                for (const auto& report : mesh_copy.preparation_report)
                    reported_area += report.cut_area;
                expect(std::abs(reported_area - areas[side]) <= 1e-12 * areas[side],
                       "reported interface area agreement");
            }
            for (int a = 0; a <= degree; ++a)
                for (int b = 0; a + b <= degree; ++b)
                    for (int z = 0; a + b + z <= degree; ++z) {
                        double volume = 0;
                        std::array<double, 2> surface{};
                        for (int side = 0; side < 2; ++side) {
                            const auto& c = sides[side];
                            for (std::size_t q = 0; q < c.vq_w.size(); ++q)
                                volume += c.vq_w[q] * std::pow(c.vq_xi[0][q], a) *
                                          std::pow(c.vq_xi[1][q], b) * std::pow(c.vq_xi[2][q], z);
                            for (std::size_t q = 0; q < c.bq_w.size(); ++q)
                                surface[side] += c.bq_w[q] * std::pow(c.bq_xi[0][q], a) *
                                                 std::pow(c.bq_xi[1][q], b) *
                                                 std::pow(c.bq_xi[2][q], z);
                        }
                        double analytic = 1.0 / ((a + 1) * (b + 1) * (z + 1));
                        expect(std::abs(volume - analytic) <= 1e-12 * analytic,
                               "complementary oblique monomial volume");
                        expect(std::abs(surface[0] - surface[1]) <= 1e-12 * surface[0],
                               "complementary oblique monomial surface");
                    }
        }
    }
    auto explicit_6 = CutOptions{};
    explicit_6.quad_degree = 6;
    auto legacy_path = build_cut_layer(original, {{{0, {1, 1, 1}, 1}}}, explicit_6);
    for (int k = 0; k < 3; ++k)
        for (std::size_t q = 0; q < default_cut.value().vq_xi[k].size(); ++q)
            expect(std::bit_cast<std::uint64_t>(default_cut.value().vq_xi[k][q]) ==
                       std::bit_cast<std::uint64_t>(legacy_path.value().vq_xi[k][q]),
                   "default point bits");
    for (std::size_t q = 0; q < default_cut.value().vq_w.size(); ++q)
        expect(std::bit_cast<std::uint64_t>(default_cut.value().vq_w[q]) ==
                   std::bit_cast<std::uint64_t>(legacy_path.value().vq_w[q]),
               "default weight bits");
    for (int k = 0; k < 3; ++k)
        for (std::size_t q = 0; q < default_cut.value().bq_xi[k].size(); ++q)
            expect(std::bit_cast<std::uint64_t>(default_cut.value().bq_xi[k][q]) ==
                       std::bit_cast<std::uint64_t>(legacy_path.value().bq_xi[k][q]),
                   "default boundary point bits");
    for (std::size_t q = 0; q < default_cut.value().bq_w.size(); ++q)
        expect(std::bit_cast<std::uint64_t>(default_cut.value().bq_w[q]) ==
                   std::bit_cast<std::uint64_t>(legacy_path.value().bq_w[q]),
               "default boundary weight bits");
    auto retained = build_tensor_grid(s, split).value();
    options.keep_void = true;
    auto kept = build_cut_layer(retained, {{{0, {1, 1, 0}, 1}}}, options);
    expect(bool(kept) && retained.attribute.size() == 16 && kept.value().void_attribute > 0,
           "keep void");
    expect(kept.value().polyhedra->volume.size() == kept.value().cut_elem.size(),
           "retained voids do not emit empty solids");
    for (auto kind : kept.value().polyhedra->cell_kind)
        expect(kind == 1, "only solid cuts have exported geometry");
    expect(std::is_sorted(cache.value().hashes.begin(), cache.value().hashes.end()),
           "cache hashes ordered");
    expect(c.gp_face.size() == brute, "default GP arrays");
    chamfer_lid_route();
    std::cout << "P1 random200 max_conservation_error=" << max_conservation_error
              << " max_fraction_error=" << max_fraction_error << '\n';
    std::cout << "G1 random=1000 degeneracies=6 cut-element ratio=" << double(cut_count) / 1006
              << " GP=" << brute << '\n';
}
