#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace chipcae::solve::detail {
// Include the boundary of a simple extruded outline, in either winding.
inline bool outline_contains(const std::vector<std::array<double, 2>>& ring, double x, double y) {
    if (ring.empty())
        return true;
    std::size_t n = ring.size();
    while (n > 1 && ring[n - 1] == ring.front())
        --n;
    bool convex = true;
    for (std::size_t j = 0; j < n && convex; ++j) {
        auto a = ring[j], b = ring[(j + 1) % n];
        double sign = 0;
        for (std::size_t k = 0; k < n; ++k) {
            const auto& p = ring[k];
            double v = (b[0] - a[0]) * (p[1] - a[1]) - (b[1] - a[1]) * (p[0] - a[0]);
            if (std::abs(v) < 1e-14)
                continue;
            if (sign * v < 0) {
                convex = false;
                break;
            }
            sign = v;
        }
    }
    if (convex) {
        // Preserve the legacy convex predicate's operation order and tolerance.
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

    auto lo = ring.front(), hi = ring.front();
    for (std::size_t j = 1; j < n; ++j)
        for (std::size_t axis = 0; axis < 2; ++axis) {
            lo[axis] = std::min(lo[axis], ring[j][axis]);
            hi[axis] = std::max(hi[axis], ring[j][axis]);
        }
    const double tolerance = 1e-12 * std::hypot(hi[0] - lo[0], hi[1] - lo[1]);
    bool inside = false;
    for (std::size_t j = 0; j < n; ++j) {
        auto a = ring[j], b = ring[(j + 1) % n];
        const double dx = b[0] - a[0], dy = b[1] - a[1];
        const double length2 = dx * dx + dy * dy;
        const double t =
            length2 > 0 ? std::clamp(((x - a[0]) * dx + (y - a[1]) * dy) / length2, 0.0, 1.0) : 0.0;
        if (std::hypot(x - (a[0] + t * dx), y - (a[1] + t * dy)) <= tolerance)
            return true;
        if ((a[1] > y) != (b[1] > y))
            if (x < a[0] + (y - a[1]) * dx / dy)
                inside = !inside;
    }
    return inside;
}
} // namespace chipcae::solve::detail
