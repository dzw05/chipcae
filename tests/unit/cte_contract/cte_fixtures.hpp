#pragma once

#include "chipcae/material/cte_convert.hpp"

// Copied from material/tests/core_math.cpp without modifying that existing test.
// Keep its original CTE tables and off-knot samples together for the contract tests.
namespace chipcae::material::test {
inline const CteTable sec{{250, 300, 350}, {1e-5, 2e-5, 3e-5}, 250};
inline const CteTable tan{{250, 300, 350}, {1e-5, 3e-5, 5e-5}, 250};
inline constexpr Real tangent_to_secant_temperature = 325.;
inline constexpr Real tangent_to_secant_expected = 2.5e-5;
inline constexpr Real secant_to_tangent_temperature = 325.;
inline constexpr Real secant_to_tangent_expected = 4e-5;
} // namespace chipcae::material::test
