#include "chipcae/geometry/geometry.hpp"
#include "chipcae/mesh/cut_cell_mesh.hpp"
#include "chipcae/mesh/layered_hex_mesh.hpp"
#include "chipcae/solvers/fem/thermal_stress.hpp"
#include "chipcae/solvers/fvm/conjugate_heat_transfer.hpp"
#include "chipcae/solvers/particles/particle_solver.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>

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
