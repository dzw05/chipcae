#pragma once

#include "chipcae/core/solver.hpp"
#include "chipcae/mesh/cut_cell_mesh.hpp"

namespace chipcae::solvers::fvm {

class ConjugateHeatTransferSolver : public Solver {
public:
    explicit ConjugateHeatTransferSolver(const mesh::CutCellMesh& mesh);

    StepResult solve() override;
    const mesh::CutCellMesh& mesh() const noexcept { return mesh_; }

private:
    const mesh::CutCellMesh& mesh_;
};

} // namespace chipcae::solvers::fvm
