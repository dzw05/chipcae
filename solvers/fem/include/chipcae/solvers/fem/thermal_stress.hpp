#pragma once

#include "chipcae/core/solver.hpp"
#include "chipcae/mesh/layered_hex_mesh.hpp"

namespace chipcae::solvers::fem {

class ThermalStressSolver : public Solver {
public:
    explicit ThermalStressSolver(const mesh::LayeredHexMesh& mesh);

    std::string_view name() const override { return "thermal_stress"; }
    StepResult advance(const LoadStep& step) override;
    const mesh::LayeredHexMesh& mesh() const noexcept { return mesh_; }

private:
    const mesh::LayeredHexMesh& mesh_;
};

} // namespace chipcae::solvers::fem
