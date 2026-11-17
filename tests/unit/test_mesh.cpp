#include "chipcae/geometry/geometry.hpp"
#include "chipcae/mesh/background/tensor_grid.hpp"
#include "chipcae/mesh/cut_cell_mesh.hpp"
#include "chipcae/mesh/layered_hex_mesh.hpp"
#include "chipcae/solvers/fem/thermal_stress.hpp"
#include "chipcae/solvers/fvm/conjugate_heat_transfer.hpp"
#include "chipcae/solvers/particles/particle_solver.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <sstream>

namespace {

int failed = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failed;
    }
}

} // namespace

int main() {
#ifdef CHIPCAE_ENABLE_KOKKOS
    Kokkos::ScopeGuard guard;
#endif
    using namespace chipcae::mesh;
    StackModel stack;
    stack.outline.box = {0, 0, 1, 1};
    Layer layer;
    layer.name = "base";
    layer.material = "solid";
    layer.thickness = 1;
    stack.layers.push_back(layer);
    GridOptions options;
    options.levels = 2;
    options.ranks = 3;
    options.nx = 4;
    options.ny = 2;
    auto too_few = build_tensor_grid(stack, options);
    expect(!too_few && too_few.error().path == "hierarchy/levels", "C_L=2 below ranks");
    options.nx = options.ny = 8;
    auto explicit_grid = build_tensor_grid(stack, options);
    expect(bool(explicit_grid), "explicit C_L=16 succeeds");
    if (explicit_grid) {
        auto& report = explicit_grid.value().grid_report;
        expect(report.column_ranks.front().size() == 16 && !report.warnings.empty(),
               "explicit small coarse warning");
        expect(!report.auto_selected && report.rule == 0 && report.N_L == 729,
               "explicit chooser report");
    }
    options.levels = 0;
    options.nx = options.ny = 17;
    auto candidates = hierarchy_candidates(stack, options);
    expect(bool(candidates) && candidates.value().size() == 31, "candidate cap L=30");
    auto choice = select_levels(candidates.value(), 1, options.ranks);
    std::ostringstream log;
    auto* previous = std::clog.rdbuf(log.rdbuf());
    auto automatic = build_tensor_grid(stack, options);
    std::clog.rdbuf(previous);
    expect(bool(automatic), "automatic grid");
    if (automatic) {
        const auto& grid = automatic.value();
        const auto& report = grid.grid_report;
        const auto& candidate = candidates.value()[choice.L];
        expect(report.levels == choice.L + 1 && report.rule == choice.rule &&
                   report.N_L == choice.N_L && report.auto_selected,
               "automatic chooser report");
        expect(report.level_axes[0][0].size() - 1 == candidate.nx0 &&
                   report.level_axes[0][1].size() - 1 == candidate.ny0,
               "automatic coarse dimensions");
        expect(report.padding[0] == (candidate.nx0 << choice.L) - 17 &&
                   report.padding[1] == (candidate.ny0 << choice.L) - 17 &&
                   report.padding[0] + report.padding[1] == candidate.padded_cells,
               "automatic per-axis padding");
        expect(log.str().find("levels=auto(L=" + std::to_string(choice.L) +
                              ", rule=" + std::to_string(choice.rule) + ")") != std::string::npos &&
                   log.str().find("run_config levels=") != std::string::npos,
               "automatic log semantics");
    }
    stack.layers[0].n_elem_min = 2;
    options.cell_budget = 600;
    auto budget_candidates = hierarchy_candidates(stack, options);
    expect(bool(budget_candidates) && !budget_candidates.value()[1].feasible,
           "padded 3D budget feasibility");
    options.levels = 2;
    auto padding_budget = build_tensor_grid(stack, options);
    expect(!padding_budget && padding_budget.error().path == "HIERARCHY_INVALID",
           "explicit padding budget failure");
    options = GridOptions{};
    for (int n : {8, 16, 32}) {
        options.nx = options.ny = n;
        options.ranks = 3;
        stack.layers[0].n_elem_min = 3 * n / 8;
        auto planned = hierarchy_candidates(stack, options);
        expect(bool(planned), "P1 candidate planning");
        if (planned) {
            auto picked = select_levels(planned.value(), 3 * n / 8, 3);
            expect(picked.L == (n == 8    ? 0
                                : n == 16 ? 1
                                          : 2) &&
                       planned.value()[picked.L].nx0 == 8 && planned.value()[picked.L].ny0 == 8 &&
                       picked.N_L == std::size_t(n == 8    ? 6069
                                                 : n == 16 ? 11271
                                                           : 21675),
                   "P1 planned selection and equation count");
        }
    }
    stack.outline.box = {0, 0, 3, 1};
    stack.layers[0].n_elem_min = 1;
    stack.layers[0].regions.push_back({"feature", "solid", "", {{1, 0, 3, 1}, {}}});
    options = GridOptions{};
    options.max_ratio = 1;
    options.levels = 2;
    auto strict = hierarchy_candidates(stack, options);
    expect(bool(strict) && strict.value()[0].feasible && !strict.value()[1].feasible,
           "valid original grading can have infeasible padding");
    auto strict_grid = build_tensor_grid(stack, options);
    expect(!strict_grid && strict_grid.error().path == "HIERARCHY_INVALID",
           "explicit infeasible padding grading");
    chipcae::mesh::CutCellMesh mesh({2, 2, 2}, {0.0, 0.0, 0.0}, {1.0, 1.0, 1.0});
    expect(mesh.cell_count() == 8, "cut-cell count");

    mesh.add_control_volume(0, {1, 10, 100, 0.4});
    mesh.add_control_volume(0, {2, 20, 200, 0.6});
    const auto cvs = mesh.control_volumes(0);
    expect(cvs.size() == 2, "multi control volumes in one cell");
    expect(cvs[0].region_id == 1 && cvs[0].material_id == 10 && cvs[0].body_id == 100,
           "first control volume ids");
    expect(cvs[1].region_id == 2 && cvs[1].volume == 0.6, "second control volume");
    expect(mesh.control_volumes(1).empty(), "untouched cell has no control volumes");

    const auto geom = chipcae::geometry::Geometry::empty();
    expect(geom.signed_distance(0, 0, 0) > 0, "placeholder SDF is outside");

    chipcae::mesh::LayeredHexMesh hex({4, 3}, {10.0, 8.0}, {{0.2, 1, 2}, {0.5, 2, 1}});
    expect(hex.nx() == 4 && hex.ny() == 3 && hex.nz() == 3, "layered hex divisions");
    expect(hex.node_count() == 5 * 4 * 4, "layered hex node count");
    expect(hex.element_count() == 4 * 3 * 3, "layered hex element count");
    expect(hex.layer_count() == 2, "layer count");

    chipcae::solvers::fvm::ConjugateHeatTransferSolver cht(mesh);
    expect(cht.advance({}).code.code() ==
               static_cast<std::uint32_t>(chipcae::ErrorCode::NOT_IMPLEMENTED),
           "FVM placeholder");

    chipcae::solvers::fem::ThermalStressSolver fem(hex);
    expect(fem.advance({}).code.code() ==
               static_cast<std::uint32_t>(chipcae::ErrorCode::NOT_IMPLEMENTED),
           "FEM placeholder");

    chipcae::solvers::particles::ParticleSolver particles(chipcae::solvers::particles::Method::mpm,
                                                          0);
    expect(particles.advance({}).code.code() ==
               static_cast<std::uint32_t>(chipcae::ErrorCode::NOT_IMPLEMENTED),
           "particle placeholder");

    if (failed != 0) {
        std::cerr << failed << " assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
