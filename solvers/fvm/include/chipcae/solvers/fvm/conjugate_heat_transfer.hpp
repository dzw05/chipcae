#pragma once

#include "chipcae/mesh/cut_cell_mesh.hpp"

namespace chipcae::solvers::fvm {

enum class SolveStatus { ok, not_implemented };

class ConjugateHeatTransferSolver {
public:
    explicit ConjugateHeatTransferSolver(const mesh::CutCellMesh& mesh);

    SolveStatus solve();
    const mesh::CutCellMesh& mesh() const noexcept { return mesh_; }

private:
    const mesh::CutCellMesh& mesh_;
};

} // namespace chipcae::solvers::fvm
