#pragma once

#include "chipcae/mesh/layered_hex_mesh.hpp"

namespace chipcae::solvers::fem {

enum class SolveStatus { ok, not_implemented };

class ThermalStressSolver {
public:
    explicit ThermalStressSolver(const mesh::LayeredHexMesh& mesh);

    SolveStatus solve();
    const mesh::LayeredHexMesh& mesh() const noexcept { return mesh_; }

private:
    const mesh::LayeredHexMesh& mesh_;
};

} // namespace chipcae::solvers::fem
