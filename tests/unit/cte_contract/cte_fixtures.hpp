#pragma once

#include "chipcae/material/cte_convert.hpp"

#include <cmath>

namespace chipcae::material::test {
inline bool near(Real actual, Real expected) {
    // Treat cancellation residuals within the zero-reference absolute tolerance as zero.
    const Real scale = std::abs(expected);
    return std::isfinite(actual) && std::isfinite(expected) &&
           std::abs(actual - expected) <= (scale <= 1e-18 ? 1e-18 : 1e-12 * scale);
}

// Canonical CTE tables and off-knot samples copied unchanged from material/tests/core_math.cpp for
// contract tests.
inline const CteTable sec{{250, 300, 350}, {1e-5, 2e-5, 3e-5}, 250};
inline const CteTable tan{{250, 300, 350}, {1e-5, 3e-5, 5e-5}, 250};
inline constexpr Real tangent_to_secant_temperature = 325.;
inline constexpr Real tangent_to_secant_expected = 2.5e-5;
inline constexpr Real secant_to_tangent_temperature = 325.;
inline constexpr Real secant_to_tangent_expected = 4e-5;
} // namespace chipcae::material::test
