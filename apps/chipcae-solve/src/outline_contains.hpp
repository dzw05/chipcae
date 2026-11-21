#pragma once

#include <array>
#include <cmath>
#include <vector>

namespace chipcae::solve::detail {
// Test-first compilation stub: retain the legacy convex-only predicate until
// nonconvex containment is implemented. This helper is not wired into the app.
inline bool outline_contains(const std::vector<std::array<double, 2>>& ring, double x, double y) {
    double sign = 0;
    for (std::size_t j = 0; j < ring.size(); ++j) {
        auto a = ring[j], b = ring[(j + 1) % ring.size()];
        double v = (b[0] - a[0]) * (y - a[1]) - (b[1] - a[1]) * (x - a[0]);
        if (std::abs(v) < 1e-14)
            continue;
        if (sign * v < 0)
            return false;
        sign = v;
    }
    return true;
}
} // namespace chipcae::solve::detail
