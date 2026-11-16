#include "chipcae/solvers/fem/thermal_stress.hpp"

#include "chipcae/core/log.hpp"

namespace chipcae::solvers::fem {

ThermalStressSolver::ThermalStressSolver(const mesh::LayeredHexMesh& mesh) : mesh_(mesh) {}

StepResult ThermalStressSolver::advance(const LoadStep&) {
    log(LogLevel::info, "FEM thermal stress and warpage solve is not implemented yet");
    StepResult result;
    result.status = StepResult::Status::Failed;
    result.code = ErrorCode::NOT_IMPLEMENTED;
    return result;
}

} // namespace chipcae::solvers::fem
