#pragma once

#include "chipcae/mesh/background/hex_mesh_arrays.hpp"

#include <cstdint>
#include <mfem.hpp>
#include <stdexcept>
#include <vector>

namespace chipcae::fem {
struct ColumnGmgUnsupported : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct ColumnGmgOptions {
    double rtol = 1e-10;
    int max_iter = 200;
    int chebyshev_degree = 16;
    double chebyshev_lower = 0.003;
    int eig_iterations = 30;
};

struct ColumnGmgLevel {
    std::vector<double> x, y;
    std::vector<std::int64_t> col_off;
    std::vector<std::int64_t> col_dof;
    std::vector<std::uint8_t> essential;
};

struct ColumnGmgHierarchy {
    int L = 0, ncomp = 1, nz_nodes = 0;
    std::vector<ColumnGmgLevel> levels;
    std::vector<mfem::SparseMatrix> P;
};

struct ColumnGmgStats {
    std::uint32_t iterations = 0;
    double relative_residual = 0, setup_seconds = 0, solve_seconds = 0;
    std::vector<std::int64_t> level_rows;
    std::vector<std::int64_t> level_blocks;
};

ColumnGmgHierarchy build_column_gmg_hierarchy(const mesh::HexMeshArrays& m,
                                              bool has_cut,
                                              int np,
                                              const mfem::FiniteElementSpace& fes,
                                              const mfem::Array<int>& essential_tdofs);

ColumnGmgStats solve_column_gmg(const mesh::HexMeshArrays& m,
                                bool has_cut,
                                int np,
                                const mfem::FiniteElementSpace& fes,
                                const mfem::SparseMatrix& A,
                                const mfem::Array<int>& essential_tdofs,
                                const mfem::Vector& b,
                                mfem::Vector& x,
                                const ColumnGmgOptions& options = {});
} // namespace chipcae::fem
