#pragma once

#include "chipcae/core/solver.hpp"
#include "chipcae/mesh/cut_cell_mesh.hpp"

namespace chipcae::solvers::fvm {

class ConjugateHeatTransferSolver : public Solver {
public:
    explicit ConjugateHeatTransferSolver(const mesh::CutCellMesh& mesh);

    std::string_view name() const override { return "conjugate_heat_transfer"; }
    StepResult advance(const LoadStep& step) override;
    const mesh::CutCellMesh& mesh() const noexcept { return mesh_; }

private:
    const mesh::CutCellMesh& mesh_;
};

} // namespace chipcae::solvers::fvm
