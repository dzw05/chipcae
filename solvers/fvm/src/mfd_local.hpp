#pragma once

#include "chipcae/solvers/fvm/grid.hpp"

#include <string>

namespace chipcae::solvers::fvm::detail {
// Minimal internal test seam for the brief's box-diagonality/geometry gates.
struct LocalMfd {
    std::vector<double> M; // Row-major local inner product, not its inverse.
    double condition = 0;
    std::int64_t gamma_floor_count = 0;
};
Expected<LocalMfd> build_local_mfd(const FvGrid&,
                                   std::size_t cv,
                                   const std::array<double, 3>& k,
                                   const std::string& stabilization);
} // namespace chipcae::solvers::fvm::detail
