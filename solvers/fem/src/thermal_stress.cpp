#include "chipcae/solvers/fem/thermal_stress.hpp"

#include "chipcae/core/log.hpp"

namespace chipcae::solvers::fem {

ThermalStressSolver::ThermalStressSolver(const mesh::LayeredHexMesh& mesh) : mesh_(mesh) {}

SolveStatus ThermalStressSolver::solve() {
    log(LogLevel::info, "FEM thermal stress and warpage solve is not implemented yet");
    return SolveStatus::not_implemented;
}

} // namespace chipcae::solvers::fem
