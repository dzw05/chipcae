#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace chipcae::solve::detail {
inline constexpr double kConvexityCrossRelTol = 1e-12;
inline constexpr double kBoundaryRelTol = 1e-12;
// Legacy convex predicate, absolute, kept for bit compatibility.
inline constexpr double kLegacyConvexCrossEps = 1e-14;

namespace outline_internal {
inline double cross(const std::array<double, 2>& a,
                    const std::array<double, 2>& b,
                    const std::array<double, 2>& p) {
    return (b[0] - a[0]) * (p[1] - a[1]) - (b[1] - a[1]) * (p[0] - a[0]);
}

struct Geometry {
    std::size_t n;
    double diagonal;
};

inline Geometry prepare(const std::vector<std::array<double, 2>>& ring) {
    if (ring.empty())
        return {0, 0};
    std::size_t n = ring.size();
    while (n > 1 && ring[n - 1] == ring.front())
        --n;
    auto lo = ring.front(), hi = ring.front();
    for (std::size_t j = 1; j < n; ++j)
        for (std::size_t axis = 0; axis < 2; ++axis) {
            lo[axis] = std::min(lo[axis], ring[j][axis]);
            hi[axis] = std::max(hi[axis], ring[j][axis]);
        }
    return {n, std::hypot(hi[0] - lo[0], hi[1] - lo[1])};
}

inline bool is_convex(const std::vector<std::array<double, 2>>& ring, const Geometry& geometry) {
    const auto n = geometry.n;
    const double threshold = kConvexityCrossRelTol * geometry.diagonal * geometry.diagonal;
    bool convex = true;
    for (std::size_t j = 0; j < n && convex; ++j) {
        auto a = ring[j], b = ring[(j + 1) % n];
        double sign = 0;
        for (std::size_t k = 0; k < n; ++k) {
            const auto& p = ring[k];
            double v = cross(a, b, p);
            if (std::abs(v) < threshold)
                continue;
            if (sign * v < 0) {
                convex = false;
                break;
            }
            sign = v;
        }
    }
    return convex;
}

inline bool contains(const std::vector<std::array<double, 2>>& ring,
                     const Geometry& geometry,
                     bool convex,
                     double x,
                     double y) {
    if (ring.empty())
        return true;
    if (convex) {
        // Preserve the legacy convex predicate's operation order and tolerance.
        double sign = 0;
        for (std::size_t j = 0; j < ring.size(); ++j) {
            auto a = ring[j], b = ring[(j + 1) % ring.size()];
            double v = cross(a, b, {x, y});
            if (std::abs(v) < kLegacyConvexCrossEps)
                continue;
            if (sign * v < 0)
                return false;
            sign = v;
        }
        return true;
    }

    const auto n = geometry.n;
    const double tolerance = kBoundaryRelTol * geometry.diagonal;
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
} // namespace outline_internal

inline bool outline_is_convex(const std::vector<std::array<double, 2>>& ring) {
    return outline_internal::is_convex(ring, outline_internal::prepare(ring));
}

// Include the boundary of a simple extruded outline, in either winding.
inline bool
outline_contains(const std::vector<std::array<double, 2>>& ring, bool convex, double x, double y) {
    return outline_internal::contains(ring, outline_internal::prepare(ring), convex, x, y);
}

inline bool outline_contains(const std::vector<std::array<double, 2>>& ring, double x, double y) {
    const auto geometry = outline_internal::prepare(ring);
    return outline_internal::contains(
        ring, geometry, outline_internal::is_convex(ring, geometry), x, y);
}
} // namespace chipcae::solve::detail
