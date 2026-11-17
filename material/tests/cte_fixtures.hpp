#pragma once

#include "chipcae/material/cte_convert.hpp"

namespace chipcae::material::test {
inline const CteTable secant_cte{{250, 300, 350}, {1e-5, 2e-5, 3e-5}, 250};
inline const CteTable tangent_cte{{250, 300, 350}, {1e-5, 3e-5, 5e-5}, 250};
} // namespace chipcae::material::test
