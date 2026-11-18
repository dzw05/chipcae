#include "chipcae/mesh/cut/plane_cut.hpp"
#include "chipcae/mesh/cut/region_cut.hpp"
#include "support.hpp"

#include <bit>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <map>
#include <string>

int main() {
    return sp5::run([] {
        using namespace chipcae::mesh;
        using namespace chipcae::mesh::cut;
        using chipcae::BodyId;
        using chipcae::Index;
        using chipcae::Real;
        using sp5::require;

        const auto threshold = [](const std::string& key) {
            std::ifstream in(std::string(CHIPCAE_SOURCE_DIR) +
                             "/mesh/background/thresholds/k1.json");
            require(bool(in), "lid region: threshold file opens");
            const std::string text((std::istreambuf_iterator<char>(in)), {});
            const auto pos = text.find('"' + key + '"');
            require(pos != std::string::npos, "lid region: threshold key exists");
            const auto colon = text.find(':', pos);
            require(colon != std::string::npos, "lid region: threshold value exists");
            return std::stod(text.substr(colon + 1));
        };
        GridOptions grid_options;
        grid_options.cell_budget = static_cast<Index>(threshold("cell_budget"));
        grid_options.snap_tol = threshold("snap_tol");
        grid_options.max_ratio = threshold("max_ratio");
        grid_options.levels = 1;
        CutOptions options;
        options.quad_degree = 6;
        options.export_polyhedra = true;
        options.snap_phi = threshold("snap_phi");
        options.small_sub_vf = threshold("small_sub_vf");
        options.keep_void = false;
        const auto volume_tolerance = threshold("cut_volume_relative");

        StackModel stack;
        stack.id = "region-chamfer-lid";
        stack.outline.box = {0, 0, 56, 44};
        Layer substrate;
        substrate.name = "substrate";
        substrate.kind = "Explicit";
        substrate.material = "solid";
        Layer ring;
        ring.name = "ring_layer";
        ring.kind = "Explicit";
        ring.regions = {{"ring_s", "solid", "ring", {{10, 10, 46, 14}, {}}},
                        {"ring_n", "solid", "ring", {{10, 30, 46, 34}, {}}},
                        {"ring_w", "solid", "ring", {{10, 14, 14, 30}, {}}},
                        {"ring_e", "solid", "ring", {{42, 14, 46, 30}, {}}}};
        Layer lid;
        lid.name = "lid_layer";
        lid.kind = "Explicit";
        const std::vector<std::array<Real, 2>> polygon{
            {2, 0}, {54, 0}, {56, 2}, {56, 42}, {54, 44}, {2, 44}, {0, 42}, {0, 2}};
        lid.regions = {{"lid", "solid", "lid", {stack.outline.box, polygon}}};
        stack.layers = {substrate, ring, lid};
        for (auto& layer : stack.layers) {
            layer.thickness = 1;
            layer.n_elem_min = 1;
        }

        const auto body_of = [](const HexMeshArrays& mesh, Local attribute) {
            require(attribute > 0 &&
                        static_cast<std::size_t>(attribute) <= mesh.attr_table.entries.size(),
                    "lid region: attribute index is valid");
            return mesh.attr_table.entries[attribute - 1].body;
        };
        const auto vertex = [](const HexMeshArrays& mesh, std::size_t e, std::size_t node) {
            require(e < mesh.attribute.size() && 8 * e + node < mesh.hex.size(),
                    "lid region: parent connectivity index is valid");
            const auto v = mesh.hex[8 * e + node];
            require(v >= 0 && 3 * static_cast<std::size_t>(v) + 2 < mesh.vertices.size(),
                    "lid region: vertex index is valid");
            return &mesh.vertices[3 * static_cast<std::size_t>(v)];
        };
        const auto jacobian = [&](const HexMeshArrays& mesh, std::size_t e) {
            const auto* lo = vertex(mesh, e, 0);
            const auto* hi = vertex(mesh, e, 6);
            const auto jac = (hi[0] - lo[0]) * (hi[1] - lo[1]) * (hi[2] - lo[2]);
            require(jac > 0, "lid region: parent volume is positive");
            return jac;
        };
        const auto sorted = [](auto values) {
            std::sort(values.begin(), values.end());
            return values;
        };
        const auto check_offsets = [](const CutLayerArrays& cut) {
            require(cut.sub_off.size() == cut.cut_elem.size() + 1 && cut.sub_off.front() == 0 &&
                        std::is_sorted(cut.sub_off.begin(), cut.sub_off.end()) &&
                        static_cast<std::size_t>(cut.sub_off.back()) == cut.sub_volfrac.size() &&
                        cut.sub_attribute.size() == cut.sub_volfrac.size(),
                    "lid region: subcell CSR offsets and attributes are valid");
            require(cut.vq_off.size() == cut.sub_volfrac.size() + 1 && cut.vq_off.front() == 0 &&
                        std::is_sorted(cut.vq_off.begin(), cut.vq_off.end()) &&
                        static_cast<std::size_t>(cut.vq_off.back()) == cut.vq_w.size(),
                    "lid region: quadrature CSR offsets are valid");
        };

        for (std::size_t resolution = 0; resolution < 2; ++resolution) {
            const bool coarse = resolution == 0;
            const Local nominal_nx = coarse ? 20 : 56, nominal_ny = coarse ? 16 : 44;
            // Whole-outline resolutions become minimum counts per feature interval (#130).
            grid_options.nx = std::max(Local{1}, 4 * nominal_nx / 56);
            grid_options.ny = std::max(Local{1}, 4 * nominal_ny / 44);
            const auto grid = build_tensor_grid(stack, grid_options);
            if (!grid)
                std::cerr << "lid region: grid " << (coarse ? 'A' : 'B') << ": "
                          << grid.error().path << ": " << grid.error().reason << '\n';
            require(bool(grid), "lid region: build_tensor_grid succeeds");
            // Explicit const binding selects the non-mutating overload for every const route.
            const HexMeshArrays& original = grid.value();
            require(original.elem_gid.size() == original.attribute.size(),
                    "lid region: every original cell has a GID");
            const auto& bodies = original.attr_table.bodies;
            require(bodies.contains("substrate") && bodies.contains("ring") &&
                        bodies.contains("lid"),
                    "lid region: substrate, ring and lid bodies exist");
            const auto substrate_id = bodies.at("substrate");
            const auto ring_id = bodies.at("ring");
            const auto lid_id = bodies.at("lid");
            require(substrate_id != 0 && ring_id != 0 && lid_id != 0 && substrate_id != ring_id &&
                        substrate_id != lid_id && ring_id != lid_id,
                    "lid region: body IDs are nonzero and pairwise distinct");
            for (const auto& [name, id] : bodies) {
                (void)id;
                auto lower = name;
                std::transform(name.begin(), name.end(), lower.begin(), [](unsigned char c) {
                    return static_cast<char>(std::tolower(c));
                });
                require(lower == name || (lower != "lid" && lower != "ring"),
                        "lid region: ring and lid names have exact capitalization");
            }
            const auto pending = sorted(original.pending_cut);
            require(!pending.empty(), "lid region: pending cuts exist");
            std::set<Index> pending_gids;
            for (const auto e : pending) {
                require(e >= 0 && static_cast<std::size_t>(e) < original.attribute.size(),
                        "lid region: pending index is valid");
                require(body_of(original, original.attribute[e]) == lid_id,
                        "lid region: only lid cells are pending");
                pending_gids.insert(original.elem_gid[e]);
            }

            const auto extracted = region_cut_planes(stack, original);
            require(bool(extracted), "lid region: region_cut_planes succeeds");
            const auto& geometry = extracted.value();
            require(geometry.planes.size() == 8, "lid region: exactly eight planes are extracted");
            for (const auto& plane : geometry.planes)
                require(plane.body == lid_id && plane.n[2] == 0,
                        "lid region: every extracted plane is vertical and scoped to lid");

            const auto diagonal = [&](std::size_t edge) {
                const auto& a = polygon[edge];
                const auto& b = polygon[(edge + 1) % polygon.size()];
                const auto dx = b[0] - a[0], dy = b[1] - a[1];
                const auto length = std::hypot(dx, dy);
                const std::array<Real, 3> normal{dy / length, -dx / length, 0};
                return Plane{lid_id, normal, normal[0] * a[0] + normal[1] * a[1]};
            };
            // Independent manual route uses the exact axis planes and CCW formula from #130.
            const CutGeometry manual_geometry{{{lid_id, {0, -1, 0}, 0},
                                               diagonal(1),
                                               {lid_id, {1, 0, 0}, 56},
                                               diagonal(3),
                                               {lid_id, {0, 1, 0}, 44},
                                               diagonal(5),
                                               {lid_id, {-1, 0, 0}, 0},
                                               diagonal(7)}};
            const auto& x = original.axes[0];
            const auto& y = original.axes[1];
            require(x.size() >= 2 && y.size() >= 2, "lid region: axes contain corner intervals");
            const std::array hx{x[1] - x.front(), x.back() - x[x.size() - 2]};
            const std::array hy{y[1] - y.front(), y.back() - y[y.size() - 2]};
            if (coarse)
                require(hx[0] > 2 && hx[1] > 2 && hy[0] > 2 && hy[1] > 2,
                        "lid region: A corner widths exceed chamfer legs");
            else
                require(std::abs(hx[0] - 1) <= 1e-12 && std::abs(hx[1] - 1) <= 1e-12 &&
                            std::abs(hy[0] - 1) <= 1e-12 && std::abs(hy[1] - 1) <= 1e-12,
                        "lid region: B has four unit corner intervals");

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
                for (const auto xc : {0., 56.})
                    for (const auto yc : {0., 44.}) {
                        // Each diagonal lies at Manhattan distance 2 from an outline corner.
                        const auto near = std::min(std::abs(lo[0] - xc), std::abs(hi[0] - xc)) +
                                          std::min(std::abs(lo[1] - yc), std::abs(hi[1] - yc));
                        const auto far = std::max(std::abs(lo[0] - xc), std::abs(hi[0] - xc)) +
                                         std::max(std::abs(lo[1] - yc), std::abs(hi[1] - yc));
                        crossed = crossed || (near < 2 && 2 < far);
                    }
                if (crossed)
                    expected_cuts.push_back(static_cast<Local>(e));
            }
            require(!lid_columns.empty() && lid_cells % lid_columns.size() == 0,
                    "lid region: lid cells divide exactly into occupied XY columns");
            const auto nz_lid = lid_cells / lid_columns.size();

            const auto result = build_cut_layer(original, geometry, options);
            require(bool(result), "lid region: const region cut succeeds");
            const auto& cut = result.value();
            auto compact = original;
            const auto compact_result = build_cut_layer(compact, geometry, options);
            require(bool(compact_result), "lid region: mutable region cut succeeds");
            const auto& compact_cut = compact_result.value();
            const auto manual_result = build_cut_layer(original, manual_geometry, options);
            require(bool(manual_result), "lid region: const manual cut succeeds");
            const auto& manual_cut = manual_result.value();
            auto manual_compact = original;
            const auto manual_compact_result =
                build_cut_layer(manual_compact, manual_geometry, options);
            require(bool(manual_compact_result), "lid region: mutable manual cut succeeds");
            const auto& manual_compact_cut = manual_compact_result.value();

            Real max_quadrature_error = 0;
            Real max_polyhedron_error = 0;
            const auto check_cut = [&](const HexMeshArrays& mesh, const CutLayerArrays& route) {
                check_offsets(route);
                require(!route.cut_elem.empty(), "lid region: cut parents exist");
                require(route.cut_elem.size() == (coarse ? 4 : 8) * nz_lid,
                        "lid region: cut parent count matches corner crossings");
                require(route.cut_elem_gid.size() == route.cut_elem.size() &&
                            mesh.elem_gid.size() == mesh.attribute.size(),
                        "lid region: parent GID arrays have matching sizes");
                require(route.polyhedra &&
                            route.polyhedra->volume.size() == route.sub_volfrac.size(),
                        "lid region: every subcell has an exported polyhedron volume");
                const auto indices = sorted(route.cut_elem);
                require(std::adjacent_find(indices.begin(), indices.end()) == indices.end(),
                        "lid region: cut parents are unique");
                for (std::size_t i = 0; i < route.cut_elem.size(); ++i) {
                    const auto e = route.cut_elem[i];
                    require(e >= 0 && static_cast<std::size_t>(e) < mesh.attribute.size(),
                            "lid region: cut parent index is valid");
                    require(mesh.elem_gid[e] == route.cut_elem_gid[i],
                            "lid region: cut index resolves to its recorded GID");
                    require(pending_gids.contains(mesh.elem_gid[e]) &&
                                body_of(mesh, mesh.attribute[e]) == lid_id,
                            "lid region: cut parent belongs to lid and was pending");
                    const auto begin = route.sub_off[i], end = route.sub_off[i + 1];
                    require(coarse ? end - begin == 1 : end > begin,
                            "lid region: each cut parent has the expected subcells");
                    const auto* lo = vertex(mesh, e, 0);
                    const auto expected = coarse ? 1 - 2 / (hx[lo[0] > 28] * hy[lo[1] > 22]) : .5;
                    const auto jac = jacobian(mesh, e);
                    for (auto sub = begin; sub < end; ++sub) {
                        require(body_of(mesh, route.sub_attribute[sub]) == lid_id,
                                "lid region: every cut subcell belongs to lid");
                        const auto vf = route.sub_volfrac[sub];
                        require(vf > 0 && std::abs(vf - expected) <= 1e-12,
                                "lid region: chamfer fraction matches analytic corner area");
                        const auto weight =
                            std::accumulate(route.vq_w.begin() + route.vq_off[sub],
                                            route.vq_w.begin() + route.vq_off[sub + 1],
                                            0.0);
                        const auto q_error = std::abs(weight - vf) / vf;
                        const auto p_error =
                            std::abs(route.polyhedra->volume[sub] - jac * vf) / (jac * vf);
                        max_quadrature_error = std::max(max_quadrature_error, q_error);
                        max_polyhedron_error = std::max(max_polyhedron_error, p_error);
                        require(std::abs(weight - vf) <= 1e-12 * vf,
                                "lid region: quadrature weights sum to subcell fraction");
                        require(std::abs(route.polyhedra->volume[sub] - jac * vf) <=
                                    1e-12 * jac * vf,
                                "lid region: polyhedron volume matches physical subcell volume");
                    }
                }
                for (const auto e : route.void_elem)
                    require(e >= 0 && static_cast<std::size_t>(e) < mesh.attribute.size() &&
                                body_of(mesh, mesh.attribute[e]) == lid_id,
                            "lid region: void index is valid and belongs to lid");
                for (const auto sub : route.small_subs)
                    require(sub >= 0 && static_cast<std::size_t>(sub) < route.sub_volfrac.size() &&
                                route.sub_volfrac[sub] < options.small_sub_vf,
                            "lid region: reported small subcell is valid and below threshold");
            };
            check_cut(original, cut);
            check_cut(compact, compact_cut);
            check_cut(original, manual_cut);
            check_cut(manual_compact, manual_compact_cut);
            require(sorted(cut.cut_elem) == expected_cuts,
                    "lid region: only strict diagonal interior crossings are cut");
            for (const auto e : cut.cut_elem)
                require(std::binary_search(pending.begin(), pending.end(), e),
                        "lid region: const cut indices belong to original pending_cut");
            if (coarse)
                require(cut.void_elem.empty(), "lid region: A has no void cells");

            const auto check_remapping = [&](const HexMeshArrays& mesh,
                                             const CutLayerArrays& mutable_cut,
                                             const CutLayerArrays& const_cut) {
                require(mutable_cut.void_elem.empty() &&
                            mesh.attribute.size() + const_cut.void_elem.size() ==
                                original.attribute.size(),
                        "lid region: mutable cut removes exactly the const void cells");
                const std::set<Local> voids(const_cut.void_elem.begin(), const_cut.void_elem.end());
                std::vector<Index> retained;
                for (std::size_t e = 0; e < original.elem_gid.size(); ++e)
                    if (!voids.contains(static_cast<Local>(e)))
                        retained.push_back(original.elem_gid[e]);
                require(mesh.elem_gid == retained,
                        "lid region: mutable mesh preserves exact retained GID sequence");
                require(sorted(mutable_cut.cut_elem_gid) == sorted(const_cut.cut_elem_gid),
                        "lid region: const and mutable cut GID sets agree");
            };
            check_remapping(compact, compact_cut, cut);
            check_remapping(manual_compact, manual_compact_cut, manual_cut);

            if (!coarse) {
                const std::set<Local> cuts(cut.cut_elem.begin(), cut.cut_elem.end());
                const std::set<Local> voids(cut.void_elem.begin(), cut.void_elem.end());
                for (const auto x0 : {0., 55.})
                    for (const auto y0 : {0., 43.}) {
                        bool found_substrate = false;
                        for (std::size_t e = 0; e < original.attribute.size(); ++e) {
                            const auto* lo = vertex(original, e, 0);
                            const auto* hi = vertex(original, e, 6);
                            if (std::abs(lo[0] - x0) > 1e-12 ||
                                std::abs(hi[0] - (x0 + 1)) > 1e-12 ||
                                std::abs(lo[1] - y0) > 1e-12 || std::abs(hi[1] - (y0 + 1)) > 1e-12)
                                continue;
                            const auto body = body_of(original, original.attribute[e]);
                            require(body != lid_id && body != ring_id,
                                    "lid region: B corner columns contain no lid or ring cells");
                            if (body != substrate_id)
                                continue;
                            require(std::abs(lo[2]) <= 1e-12 && std::abs(hi[2] - 1) <= 1e-12,
                                    "lid region: B substrate corner spans z zero to one");
                            found_substrate = true;
                            const auto local = static_cast<Local>(e);
                            require(!std::binary_search(pending.begin(), pending.end(), local) &&
                                        !cuts.contains(local) && !voids.contains(local),
                                    "lid region: B substrate corner remains a full uncut cell");
                        }
                        require(found_substrate, "lid region: B substrate corner box exists");
                    }
            }

            const std::array<BodyId, 3> ids{substrate_id, ring_id, lid_id};
            constexpr std::array<long double, 3> analytic{2464, 416, 2456};
            const auto volumes = [&](const HexMeshArrays& mesh, const CutLayerArrays& route) {
                const std::set<Local> voids(route.void_elem.begin(), route.void_elem.end());
                std::map<Local, std::size_t> parents;
                for (std::size_t i = 0; i < route.cut_elem.size(); ++i)
                    parents.emplace(route.cut_elem[i], i);
                std::map<BodyId, long double> totals;
                // Accumulate in mesh order, including complete substrate corner cells.
                for (std::size_t e = 0; e < mesh.attribute.size(); ++e) {
                    const auto local = static_cast<Local>(e);
                    if (voids.contains(local))
                        continue;
                    const auto jac = jacobian(mesh, e);
                    const auto it = parents.find(local);
                    if (it == parents.end()) {
                        totals[body_of(mesh, mesh.attribute[e])] += jac;
                    } else {
                        const auto i = it->second;
                        for (auto sub = route.sub_off[i]; sub < route.sub_off[i + 1]; ++sub)
                            totals[body_of(mesh, route.sub_attribute[sub])] +=
                                jac * route.sub_volfrac[sub];
                    }
                }
                std::array<long double, 3> result{};
                for (std::size_t i = 0; i < ids.size(); ++i) {
                    result[i] = totals[ids[i]];
                    require(std::abs(result[i] - analytic[i]) <= volume_tolerance * analytic[i],
                            "lid region: body volume matches substrate 2464, ring 416, lid 2456");
                }
                return result;
            };
            const auto region_volumes = volumes(original, cut);
            const auto compact_volumes = volumes(compact, compact_cut);
            const auto manual_volumes = volumes(original, manual_cut);
            const auto manual_compact_volumes = volumes(manual_compact, manual_compact_cut);
            for (std::size_t i = 0; i < ids.size(); ++i) {
                require(std::abs(region_volumes[i] - manual_volumes[i]) <= 1e-15L * analytic[i] &&
                            std::abs(compact_volumes[i] - manual_compact_volumes[i]) <=
                                1e-15L * analytic[i],
                        "lid region: manual and region body volumes agree for both overloads");
            }

            const auto compare_parents = [&](const CutLayerArrays& a, const CutLayerArrays& b) {
                check_offsets(a);
                check_offsets(b);
                require(sorted(a.cut_elem) == sorted(b.cut_elem),
                        "lid region: routes cut the same parent index set");
                Real maximum = 0;
                for (std::size_t i = 0; i < a.cut_elem.size(); ++i) {
                    const auto it = std::find(b.cut_elem.begin(), b.cut_elem.end(), a.cut_elem[i]);
                    require(it != b.cut_elem.end(), "lid region: comparison parent exists");
                    const auto j = static_cast<std::size_t>(it - b.cut_elem.begin());
                    auto av = sorted(std::vector<Real>(a.sub_volfrac.begin() + a.sub_off[i],
                                                       a.sub_volfrac.begin() + a.sub_off[i + 1]));
                    auto bv = sorted(std::vector<Real>(b.sub_volfrac.begin() + b.sub_off[j],
                                                       b.sub_volfrac.begin() + b.sub_off[j + 1]));
                    require(av.size() == bv.size(),
                            "lid region: comparison parent subcell counts agree");
                    for (std::size_t sub = 0; sub < av.size(); ++sub) {
                        const auto difference = std::abs(av[sub] - bv[sub]);
                        require(difference <= 1e-15,
                                "lid region: sorted per-parent fractions agree within 1e-15");
                        maximum = std::max(maximum, difference);
                    }
                }
                return maximum;
            };
            const auto max_manual_difference = std::max(
                compare_parents(cut, manual_cut), compare_parents(compact_cut, manual_compact_cut));
            require(sorted(cut.cut_elem_gid) == sorted(manual_cut.cut_elem_gid) &&
                        sorted(compact_cut.cut_elem_gid) == sorted(manual_compact_cut.cut_elem_gid),
                    "lid region: manual and region cut GID sets agree for both overloads");

            const auto replay = build_cut_layer(original, geometry, options);
            require(bool(replay), "lid region: const replay succeeds");
            const auto same_bits = [](const auto& a, const auto& b, const char* message) {
                require(a.size() == b.size(), message);
                for (std::size_t i = 0; i < a.size(); ++i)
                    require(std::bit_cast<std::uint64_t>(a[i]) ==
                                std::bit_cast<std::uint64_t>(b[i]),
                            message);
            };
            same_bits(cut.vq_w,
                      replay.value().vq_w,
                      "lid region: replay preserves quadrature length and bits");
            same_bits(cut.sub_volfrac,
                      replay.value().sub_volfrac,
                      "lid region: replay preserves fraction length and bits");
            auto reversed_geometry = geometry;
            std::reverse(reversed_geometry.planes.begin(), reversed_geometry.planes.end());
            const auto reversed = build_cut_layer(original, reversed_geometry, options);
            require(bool(reversed), "lid region: reversed plane cut succeeds");
            compare_parents(cut, reversed.value());

            std::clog << std::setprecision(17) << "lid region " << (coarse ? 'A' : 'B')
                      << " 实测 xy_cells=" << x.size() - 1 << 'x' << y.size() - 1
                      << " nz_lid=" << nz_lid << " cut_parents=" << cut.cut_elem.size()
                      << " cut_subcells=" << cut.sub_volfrac.size();
            const std::array routes{
                region_volumes, compact_volumes, manual_volumes, manual_compact_volumes};
            const std::array names{" region_const_errors=",
                                   " region_mutable_errors=",
                                   " manual_const_errors=",
                                   " manual_mutable_errors="};
            for (std::size_t route = 0; route < routes.size(); ++route) {
                std::clog << names[route];
                for (std::size_t i = 0; i < ids.size(); ++i)
                    std::clog << (i == 0 ? "" : ",")
                              << std::abs(routes[route][i] - analytic[i]) / analytic[i];
            }
            std::clog << " max_manual_fraction_difference=" << max_manual_difference
                      << " max_quadrature_relative_error=" << max_quadrature_error
                      << " max_polyhedron_relative_error=" << max_polyhedron_error << '\n';
        }
    });
}
