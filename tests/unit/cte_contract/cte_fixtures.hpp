#pragma once

#include "chipcae/material/cte_convert.hpp"

#include <cmath>

namespace chipcae::material::test {
// Mirror the existing tables and 325 K expectations in read-only
// material/tests/core_math.cpp at brief baseline fe27d00 (CTE merge b037e62);
// keep these fixture numbers synchronized manually.
inline const CteTable sec{{250, 300, 350}, {1e-5, 2e-5, 3e-5}, 250};
inline const CteTable tan{{250, 300, 350}, {1e-5, 3e-5, 5e-5}, 250};
inline constexpr Real secant_to_tangent_temperature = 325.;
inline constexpr Real secant_to_tangent_expected = 4e-5;
inline constexpr Real tangent_to_secant_temperature = 325.;
inline constexpr Real tangent_to_secant_expected = 2.5e-5;

inline bool near(Real actual, Real expected) {
    // Scale by the reference only, as required by the contract comparison.
    const Real scale = std::abs(expected);
    return std::isfinite(actual) && std::isfinite(expected) &&
           std::abs(actual - expected) <= (expected == 0 ? 1e-18 : 1e-12 * scale);
}

} // namespace chipcae::material::test
