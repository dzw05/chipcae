#pragma once

#include "cte_baseline.hpp"

#include <cmath>

namespace chipcae::material::test {
inline bool near(Real actual, Real expected) {
    // Scale by the reference only, as required by the contract comparison.
    const Real scale = std::abs(expected);
    return std::isfinite(actual) && std::isfinite(expected) &&
           std::abs(actual - expected) <= (expected == 0 ? 1e-18 : 1e-12 * scale);
}

} // namespace chipcae::material::test
