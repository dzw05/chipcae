#pragma once
#include "hex_mesh_arrays.hpp"
namespace chipcae::mesh {
// Elements and nodes use one global numbering: column Morton index × nz + k
// (nodes use nz+1). par's structured (k*ny+j)*nx+i order stays inside par.
struct GridOptions {
    Real snap_tol = 1e-14, max_ratio = 2;
    Index cell_budget = 50000;
    Local ranks = 1, nx = 1, ny = 1;
    // Accepted for compatibility. METIS vs serpentine is par's compile-time choice.
    bool use_metis = false;
};
Expected<HexMeshArrays> build_tensor_grid(const StackModel&, const GridOptions&);
} // namespace chipcae::mesh
