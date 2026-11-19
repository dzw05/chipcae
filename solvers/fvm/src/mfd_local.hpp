#pragma once
#include "chipcae/solvers/fvm/grid.hpp"

#include <string>
namespace chipcae::solvers::fvm::detail {
using Dense = std::vector<long double>;
struct LocalMfd {
    std::vector<double> M;
    Dense L, b, S;
    long double alpha = 0;
    double condition = 0;
    std::int64_t gamma_floor_count = 0;
    Dense apply(const Dense&) const;
};
Expected<double> geometry_defect(const FvGrid&, std::size_t);
Expected<LocalMfd>
build_local_mfd(const FvGrid&, std::size_t, const std::array<double, 3>&, const std::string&);
} // namespace chipcae::solvers::fvm::detail
