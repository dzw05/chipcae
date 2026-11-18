#pragma once

#include "chipcae/solvers/fem/column_gmg.hpp"

namespace chipcae::solve {
// Validate before assembly; only supported GMG systems require unit essential diagonals.
inline void prepare_column_gmg(const mesh::HexMeshArrays& arrays,
                               bool has_cut,
                               int np,
                               mfem::BilinearForm& form,
                               const mfem::Array<int>& essential) {
    (void)fem::build_column_gmg_hierarchy(arrays, has_cut, np, *form.FESpace(), essential);
    form.SetDiagonalPolicy(mfem::Operator::DIAG_ONE);
}
} // namespace chipcae::solve
