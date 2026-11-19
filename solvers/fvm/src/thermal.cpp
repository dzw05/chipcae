#include "chipcae/solvers/fvm/thermal.hpp"

namespace chipcae::solvers::fvm {
Expected<ThermalResult>
solve_thermal(const FvGrid&, const mesh::HexMeshArrays&, const ThermalProblem&, MPI_Comm) {
    return mesh::Error{"fvm/thermal", "NOT_IMPLEMENTED: FVM thermal solve (test-first stub)"};
}
} // namespace chipcae::solvers::fvm
