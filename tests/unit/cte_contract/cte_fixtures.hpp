#pragma once

#include "chipcae/material/cte_convert.hpp"

#include <cmath>

namespace chipcae::material::test {
// Reuse the illustrative tables and 325 K expectations from material/tests/core_math.cpp.
// Keep these literals identical to that read-only source; no conversion implementation is copied.
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
