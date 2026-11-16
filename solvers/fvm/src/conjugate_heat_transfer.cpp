#include "chipcae/solvers/fvm/conjugate_heat_transfer.hpp"

#include "chipcae/core/log.hpp"

namespace chipcae::solvers::fvm {

ConjugateHeatTransferSolver::ConjugateHeatTransferSolver(const mesh::CutCellMesh& mesh)
    : mesh_(mesh) {}

StepResult ConjugateHeatTransferSolver::advance(const LoadStep&) {
    log(LogLevel::info, "FVM conjugate heat transfer solve is not implemented yet");
    StepResult result;
    result.status = StepResult::Status::Failed;
    result.code = ErrorCode::NOT_IMPLEMENTED;
    return result;
}

} // namespace chipcae::solvers::fvm
