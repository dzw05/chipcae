#pragma once
#include "hex_mesh_arrays.hpp"
namespace chipcae::mesh {
struct GridOptions {
    Real snap_tol = 1e-14, max_ratio = 2;
    Index cell_budget = 50000;
    Local ranks = 1, nx = 1, ny = 1;
    bool use_metis = false;
};
Expected<HexMeshArrays> build_tensor_grid(const StackModel&, const GridOptions&);
} // namespace chipcae::mesh
