#pragma once

#include "chipcae/solvers/fem/column_gmg.hpp"

#include <petscmat.h>

namespace chipcae::fem::detail {
void verify_layout(const ColumnGmgLevel& level, int slots, int rows);
void verify_operator(Mat a, const ColumnGmgLevel& level, int k, bool positive);
} // namespace chipcae::fem::detail
