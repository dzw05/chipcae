#include "chipcae/mesh/cut/cut_stats.hpp"
#include "chipcae/mesh/cut/region_cut.hpp"
#include "support.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace {
using chipcae::BodyId;
using chipcae::Real;
using namespace chipcae::mesh;
using namespace chipcae::mesh::cut;
using k1::expect;

template <class T>
T require_value(Expected<T> result, const char* message) {
    if (!result)
        std::cerr << result.error().path << ": " << result.error().reason << '\n';
    expect(bool(result), message);
    return std::move(result.value());
}

struct CellXY {
    Real hx, hy;
    bool corner;
};

CellXY classify_cell(const HexMeshArrays& mesh,
                     std::size_t cell,
                     const std::array<Real, 4>& outline,
                     Real snap_tol) {
    std::array<Real, 2> lo{std::numeric_limits<Real>::infinity(),
                           std::numeric_limits<Real>::infinity()};
    std::array<Real, 2> hi{-std::numeric_limits<Real>::infinity(),
                           -std::numeric_limits<Real>::infinity()};
    for (std::size_t node = 0; node < 8; ++node) {
        const auto index = mesh.hex[8 * cell + node];
        expect(index >= 0 && static_cast<std::size_t>(index) < mesh.vertices.size() / 3,
               "demo-gate mesh: node index in range");
        for (std::size_t axis = 0; axis < 2; ++axis) {
            const Real x = mesh.vertices[3 * static_cast<std::size_t>(index) + axis];
            lo[axis] = std::min(lo[axis], x);
            hi[axis] = std::max(hi[axis], x);
        }
    }
    const bool touches_x =
        std::abs(lo[0] - outline[0]) <= snap_tol || std::abs(hi[0] - outline[2]) <= snap_tol;
    const bool touches_y =
        std::abs(lo[1] - outline[1]) <= snap_tol || std::abs(hi[1] - outline[3]) <= snap_tol;
    return {hi[0] - lo[0], hi[1] - lo[1], touches_x && touches_y};
}

bool in_range(Local cell, std::size_t count) {
    return cell >= 0 && static_cast<std::size_t>(cell) < count;
}

std::uint64_t bits(Real value) {
    return std::bit_cast<std::uint64_t>(value);
}
} // namespace

int main() try {
    // Synthetic SI fixture for the mesh volume gate in §3.8a.
    constexpr Real chamfer = 0.002;
    constexpr Real volume = 6.272e-7;
    const std::array<std::string, 3> body_names{"substrate", "bump", "die"};
    const std::array<std::string, 3> layer_names{"substrate", "bump_eq", "die_layer"};
    const std::array<std::string, 3> region_names{"substrate_r", "bump_r", "die_r"};
    const std::array<Real, 3> thicknesses{0.001, 0.0001, 0.0005};
    const std::array<Real, 3> body_volumes{3.92e-7, 3.92e-8, 1.96e-7};
    StackModel stack;
    stack.id = "demo_gate_mesh";
    stack.unit = "m";
    stack.outline.box = {0, 0, 0.02, 0.02};
    for (std::size_t i = 0; i < body_names.size(); ++i) {
        Layer layer;
        layer.name = layer_names[i];
        layer.kind = "Explicit";
        layer.thickness = thicknesses[i];
        layer.n_elem_min = 1;
        Footprint footprint;
        footprint.box = stack.outline.box;
        footprint.polygon = {{0.002, 0},
                             {0.018, 0},
                             {0.02, 0.002},
                             {0.02, 0.018},
                             {0.018, 0.02},
                             {0.002, 0.02},
                             {0, 0.018},
                             {0, 0.002}};
        layer.regions.push_back({region_names[i], "solid", body_names[i], footprint});
        stack.layers.push_back(std::move(layer));
    }
    auto options = k1::options();
    options.nx = options.ny = 4;
    options.levels = 1;
    const HexMeshArrays mesh = require_value(build_tensor_grid(stack, options),
                                             "demo-gate mesh: build tensor grid succeeds");
    expect(mesh.axes[0].size() == 5 && mesh.axes[1].size() == 5 && mesh.axes[2].size() == 4,
           "demo-gate mesh: four xy intervals and three z intervals");
    const auto& bodies = mesh.attr_table.bodies;
    for (const auto& name : body_names)
        expect(bodies.contains(name), "demo-gate mesh: required body name exists");
    const std::array<BodyId, 3> ids{bodies.at("substrate"), bodies.at("bump"), bodies.at("die")};
    expect(ids[0] != 0 && ids[1] != 0 && ids[2] != 0, "demo-gate mesh: body ids are nonzero");
    expect(ids[0] != ids[1] && ids[0] != ids[2] && ids[1] != ids[2],
           "demo-gate mesh: body ids are distinct");
    for (const auto& [name, id] : bodies) {
        (void)id;
        auto folded = name;
        for (auto& letter : folded)
            letter = static_cast<char>(std::tolower(static_cast<unsigned char>(letter)));
        for (const auto& target : body_names)
            if (folded == target)
                expect(name == target, "demo-gate mesh: body name preserves exact case");
    }

    expect(mesh.hex.size() == 8 * mesh.attribute.size(), "demo-gate mesh: eight nodes per element");
    std::vector<CellXY> cells;
    std::cout.precision(17);
    for (std::size_t cell = 0; cell < mesh.attribute.size(); ++cell) {
        const auto xy = classify_cell(mesh, cell, stack.outline.box, options.snap_tol);
        cells.push_back(xy);
        if (xy.corner) {
            std::cout << "measured corner cell=" << cell << " hx=" << xy.hx << " hy=" << xy.hy
                      << std::endl;
            expect(xy.hx > chamfer && xy.hy > chamfer,
                   "demo-gate mesh: corner cell spans exceed chamfer");
        }
    }
    for (const auto& layer : stack.layers)
        expect(!layer.regions.front().footprint.polygon.empty(),
               "demo-gate mesh: positive region polygon is nonempty");
    const auto geometry =
        require_value(region_cut_planes(stack, mesh), "demo-gate mesh: region planes succeed");
    expect(geometry.planes.size() == 24, "demo-gate mesh: twenty-four region planes");
    std::map<BodyId, std::size_t> plane_counts;
    for (const auto& plane : geometry.planes) {
        ++plane_counts[plane.body];
        expect(plane.n[2] == 0, "demo-gate mesh: region plane has zero z normal");
    }
    std::vector<std::pair<BodyId, std::size_t>> expected_counts;
    for (const auto id : ids)
        expected_counts.emplace_back(id, 8);
    std::sort(expected_counts.begin(), expected_counts.end());
    const std::vector<std::pair<BodyId, std::size_t>> actual_counts(plane_counts.begin(),
                                                                    plane_counts.end());
    expect(actual_counts == expected_counts, "demo-gate mesh: eight planes per required body");

    CutOptions cut_options;
    cut_options.quad_degree = 6;
    cut_options.export_polyhedra = false;
    cut_options.keep_void = false;
    cut_options.snap_phi = k1::threshold("snap_phi");
    cut_options.small_sub_vf = k1::threshold("small_sub_vf");
    const HexMeshArrays& original = mesh;
    const auto cut = require_value(build_cut_layer(original, geometry, cut_options),
                                   "demo-gate mesh: const cut succeeds");
    expect(!cut.cut_elem.empty(), "demo-gate mesh: const cut elements are nonempty");
    for (const auto cell : cut.cut_elem)
        expect(in_range(cell, cells.size()), "demo-gate mesh: cut index in original mesh range");
    for (const auto cell : cut.void_elem)
        expect(in_range(cell, cells.size()), "demo-gate mesh: void index in original mesh range");
    std::vector<bool> is_cut(cells.size(), false);
    for (const auto cell : cut.cut_elem) {
        const auto index = static_cast<std::size_t>(cell);
        is_cut[index] = true;
        expect(cells[index].corner, "demo-gate mesh: cut element belongs to a corner column");
        expect(cells[index].hx > chamfer && cells[index].hy > chamfer,
               "demo-gate mesh: cut cell spans exceed chamfer");
    }
    for (std::size_t cell = 0; cell < cells.size(); ++cell)
        if (!cells[cell].corner)
            expect(!is_cut[cell], "demo-gate mesh: noncorner element is not cut");
    const auto stats =
        require_value(cut_stats(original, cut), "demo-gate mesh: const cut statistics succeed");
    std::cout << "measured nx=" << mesh.axes[0].size() - 1 << " ny=" << mesh.axes[1].size() - 1
              << " cut_cells=" << stats.cut_cells << " solid_volume=" << stats.solid_volume
              << " relative_error=" << std::abs(stats.solid_volume - volume) / volume << std::endl;
    expect(stats.cut_cells > 0 && stats.cut_cells == cut.cut_elem.size(),
           "demo-gate mesh: statistics count nonempty cut elements");
    const Real relative_gate = k1::threshold("cut_volume_relative");
    expect(std::abs(stats.solid_volume - volume) <= relative_gate * volume,
           "demo-gate mesh: analytic total volume");
    for (std::size_t i = 0; i < ids.size(); ++i) {
        expect(stats.body_volume.contains(ids[i]), "demo-gate mesh: statistics contain body");
        const Real actual = stats.body_volume.at(ids[i]);
        std::cout << "measured body=" << body_names[i] << " volume=" << actual
                  << " relative_error=" << std::abs(actual - body_volumes[i]) / body_volumes[i]
                  << std::endl;
        expect(std::abs(actual - body_volumes[i]) <= relative_gate * body_volumes[i],
               "demo-gate mesh: analytic body volume");
    }
    Real body_sum = 0;
    for (const auto& [id, body_volume] : stats.body_volume) {
        (void)id;
        body_sum += body_volume;
    }
    expect(std::abs(body_sum - stats.solid_volume) <= 1e-12 * stats.solid_volume,
           "demo-gate mesh: body volumes conserve total volume");

    auto mutable_mesh = original;
    const auto mutable_cut = require_value(build_cut_layer(mutable_mesh, geometry, cut_options),
                                           "demo-gate mesh: mutable cut succeeds");
    const auto mutable_stats = require_value(cut_stats(mutable_mesh, mutable_cut),
                                             "demo-gate mesh: mutable cut statistics succeed");
    expect(mutable_stats.void_cells == 0, "demo-gate mesh: mutable cut removes void cells");
    expect(original.elem_gid.size() == cells.size(), "demo-gate mesh: original gid count");
    std::vector<bool> is_void(cells.size(), false);
    for (const auto cell : cut.void_elem)
        is_void[static_cast<std::size_t>(cell)] = true;
    std::vector<chipcae::Index> retained_gids;
    for (std::size_t cell = 0; cell < cells.size(); ++cell)
        if (!is_void[cell])
            retained_gids.push_back(original.elem_gid[cell]);
    expect(mutable_mesh.elem_gid == retained_gids,
           "demo-gate mesh: mutable gids equal original nonvoid sequence");
    auto next = original.elem_gid.begin();
    for (const auto gid : mutable_mesh.elem_gid) {
        next = std::find(next, original.elem_gid.end(), gid);
        expect(next != original.elem_gid.end(),
               "demo-gate mesh: mutable gids preserve subsequence");
        ++next;
    }
    expect(mutable_stats.cut_cells == stats.cut_cells && mutable_stats.sub_cells == stats.sub_cells,
           "demo-gate mesh: mutable and const counts agree");
    expect(std::abs(mutable_stats.solid_volume - stats.solid_volume) <= 1e-15 * volume,
           "demo-gate mesh: mutable and const volumes agree");

    const auto repeated_cut = require_value(build_cut_layer(original, geometry, cut_options),
                                            "demo-gate mesh: repeated const cut succeeds");
    const auto repeated_stats = require_value(cut_stats(original, repeated_cut),
                                              "demo-gate mesh: repeated statistics succeed");
    expect(bits(repeated_stats.solid_volume) == bits(stats.solid_volume),
           "demo-gate mesh: repeated solid volume is bitwise identical");
    expect(repeated_cut.sub_volfrac.size() == cut.sub_volfrac.size(),
           "demo-gate mesh: repeated fraction count agrees");
    for (std::size_t sub = 0; sub < cut.sub_volfrac.size(); ++sub)
        expect(bits(repeated_cut.sub_volfrac[sub]) == bits(cut.sub_volfrac[sub]),
               "demo-gate mesh: repeated fractions are bitwise identical");

    auto reversed_geometry = geometry;
    std::reverse(reversed_geometry.planes.begin(), reversed_geometry.planes.end());
    const auto reversed_cut =
        require_value(build_cut_layer(original, reversed_geometry, cut_options),
                      "demo-gate mesh: reversed plane cut succeeds");
    const auto reversed_stats = require_value(cut_stats(original, reversed_cut),
                                              "demo-gate mesh: reversed statistics succeed");
    expect(reversed_stats.cut_cells == stats.cut_cells,
           "demo-gate mesh: reversed plane cut count agrees");
    expect(std::abs(reversed_stats.solid_volume - stats.solid_volume) <= 1e-15 * volume,
           "demo-gate mesh: reversed plane volume agrees");

    auto empty_stack = stack;
    for (auto& layer : empty_stack.layers)
        layer.regions.front().footprint.polygon.clear();
    const HexMeshArrays empty_mesh = require_value(build_tensor_grid(empty_stack, options),
                                                   "demo-gate mesh: negative grid succeeds");
    const auto empty_geometry = require_value(region_cut_planes(empty_stack, empty_mesh),
                                              "demo-gate mesh: negative region planes succeed");
    expect(empty_geometry.planes.empty(), "demo-gate mesh: empty polygons produce no planes");
    const auto empty_cut = require_value(build_cut_layer(empty_mesh, empty_geometry, cut_options),
                                         "demo-gate mesh: negative cut succeeds");
    const auto empty_stats = require_value(cut_stats(empty_mesh, empty_cut),
                                           "demo-gate mesh: negative statistics succeed");
    expect(empty_stats.cut_cells == 0, "demo-gate mesh: empty polygons produce no cut cells");
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
