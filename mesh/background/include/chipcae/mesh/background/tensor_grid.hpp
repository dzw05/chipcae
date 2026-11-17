#pragma once
#include "hex_mesh_arrays.hpp"

#include <limits>
#include <vector>
namespace chipcae::mesh {
// Elements and nodes use one global numbering: column Morton index × nz + k
// (nodes use nz+1). par's structured (k*ny+j)*nx+i order stays inside par.
struct GridOptions {
    Real snap_tol = 1e-14, max_ratio = 2;
    // Optional diagnostic thresholds, applied only to finest cells.
    Real ar_warn = std::numeric_limits<Real>::infinity();
    Real ar_max = std::numeric_limits<Real>::infinity();
    Index cell_budget = 50000;
    Local ranks = 1, nx = 1, ny = 1;
    Local levels = 0;             // 0 selects automatically; otherwise L = levels - 1.
    Index max_coarse_columns = 0; // Soft solver limit; zero means unlimited.
    // Deprecated compatibility field; par's compile-time choice controls partitioning.
    bool use_metis = false;
};
namespace detail {
Expected<Real> column_partition_imbalance(const std::vector<std::vector<Local>>& column_ranks,
                                          std::size_t nz,
                                          int ranks);
} // namespace detail
Expected<HexMeshArrays> build_tensor_grid(const StackModel&, const GridOptions&);
} // namespace chipcae::mesh
