#pragma once

#include "chipcae/material/cte_convert.hpp"

// Checked copy of material/tests/core_math.cpp sec/tan fixtures and scalar checks.
// Baseline: fe27d00 (CTE conversion merged in b037e62). Keep these values in sync
// with that read-only source; generic probes derive temperatures from each table.
// The originals are local variables in core_math.cpp's main(), not exported fixtures.
// Direct shared reuse needs a brief scope change to extract a material test header;
// including that translation unit would also import its unrelated tests and main().
namespace chipcae::material::test {
inline const CteTable sec{{250, 300, 350}, {1e-5, 2e-5, 3e-5}, 250};
inline const CteTable tan{{250, 300, 350}, {1e-5, 3e-5, 5e-5}, 250};
inline constexpr Real secant_to_tangent_temperature = 325.;
inline constexpr Real secant_to_tangent_expected = 4e-5;
inline constexpr Real tangent_to_secant_temperature = 325.;
inline constexpr Real tangent_to_secant_expected = 2.5e-5;
} // namespace chipcae::material::test
