#pragma once

#include "chipcae/core/solver.hpp"
#include "chipcae/mesh/layered_hex_mesh.hpp"

namespace chipcae::solvers::fem {

class ThermalStressSolver : public Solver {
public:
    explicit ThermalStressSolver(const mesh::LayeredHexMesh& mesh);

    StepResult solve() override;
    const mesh::LayeredHexMesh& mesh() const noexcept { return mesh_; }

private:
    const mesh::LayeredHexMesh& mesh_;
};

} // namespace chipcae::solvers::fem
