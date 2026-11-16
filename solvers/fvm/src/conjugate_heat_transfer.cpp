#include "chipcae/solvers/fvm/conjugate_heat_transfer.hpp"

#include "chipcae/core/log.hpp"

namespace chipcae::solvers::fvm {

ConjugateHeatTransferSolver::ConjugateHeatTransferSolver(const mesh::CutCellMesh& mesh)
    : mesh_(mesh) {}

SolveStatus ConjugateHeatTransferSolver::solve() {
    log(LogLevel::info, "FVM conjugate heat transfer solve is not implemented yet");
    return SolveStatus::not_implemented;
}

} // namespace chipcae::solvers::fvm
