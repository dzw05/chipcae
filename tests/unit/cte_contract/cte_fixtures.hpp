#pragma once

#include "chipcae/material/cte_convert.hpp"

#include <cmath>

namespace chipcae::material::test {
// Exact copies of the local sec/tan fixtures and scalar sample checks in
// material/tests/core_math.cpp, PR #9, commit b037e62. Those fixtures live in
// main() and cannot be included directly. Keep these copies synchronized with
// that source; no configure-time source parsing is needed.
inline const CteTable sec{{250, 300, 350}, {1e-5, 2e-5, 3e-5}, 250};
inline const CteTable tan{{250, 300, 350}, {1e-5, 3e-5, 5e-5}, 250};
inline constexpr Real secant_to_tangent_temperature = 325.;
inline constexpr Real secant_to_tangent_expected = 4e-5;
inline constexpr Real tangent_to_secant_temperature = 325.;
inline constexpr Real tangent_to_secant_expected = 2.5e-5;

inline bool near(Real actual, Real expected) {
    // Scale by the reference only, as required by the contract comparison.
    // Treat cancellation residuals within the zero-reference absolute tolerance as zero.
    const Real scale = std::abs(expected);
    return std::isfinite(actual) && std::isfinite(expected) &&
           std::abs(actual - expected) <= (scale <= 1e-18 ? 1e-18 : 1e-12 * scale);
}

} // namespace chipcae::material::test
