#pragma once

#include "canonical_cte_fixtures.hpp"
#include "chipcae/material/cte_convert.hpp"

#include <cmath>

namespace chipcae::material::test {
inline bool near(Real actual, Real expected) {
    // Scale by the reference only, as required by the contract comparison.
    // Treat cancellation residuals within the zero-reference absolute tolerance as zero.
    const Real scale = std::abs(expected);
    return std::isfinite(actual) && std::isfinite(expected) &&
           std::abs(actual - expected) <= (scale <= 1e-18 ? 1e-18 : 1e-12 * scale);
}

} // namespace chipcae::material::test
