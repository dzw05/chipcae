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
    // CG stops against ||b||_2; the reported residual uses max(||b||_2, 1).
    double rtol = 1e-10;
    int max_iter = 200;
    int chebyshev_degree = 16;
    double chebyshev_lower = 0.003;
    int eig_iterations = 30;
};

struct ColumnGmgLevel {
    // Q2 lattice axes. Blocks follow morton2d(ix, iy); slots follow z, then component.
    std::vector<double> x, y;
    // Operator offsets retain empty blocks after coarse essential DOFs are removed.
    std::vector<std::int64_t> col_off;
    // Finest: operator row -> MFEM tdof. Coarser: operator row -> full-layout index.
    std::vector<std::int64_t> col_dof;
    // Full-layout mask: essential_tdofs at the finest level, empty P columns below it.
    std::vector<std::uint8_t> essential;
};

struct ColumnGmgHierarchy {
    int L = 0, ncomp = 1, nz_nodes = 0;
    std::vector<ColumnGmgLevel> levels; // Coarsest (0) through finest (L).
    std::vector<mfem::SparseMatrix> P;  // P[k]: level k operator -> level k+1 operator.
};

struct ColumnGmgStats {
    std::uint32_t iterations = 0;
    double relative_residual = 0, setup_seconds = 0, solve_seconds = 0;
    std::vector<std::int64_t> level_rows;
    std::vector<std::int64_t> level_blocks; // Smoothed levels 1..L only.
};

ColumnGmgHierarchy build_column_gmg_hierarchy(const mesh::HexMeshArrays& m,
                                              bool has_cut,
                                              int np,
                                              const mfem::FiniteElementSpace& fes,
                                              const mfem::Array<int>& essential_tdofs);

// A is the FormLinearSystem matrix, with DIAG_ONE for essential tdofs.
// Unsupported configurations throw ColumnGmgUnsupported; solver failures remain runtime errors.
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
