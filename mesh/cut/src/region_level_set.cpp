#include "chipcae/mesh/cut/region_level_set.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>

namespace chipcae::mesh {
namespace {
using Point = std::array<Real, 2>;
using Polygon = std::vector<Point>;
Real distance(Point p, Point a, Point b) {
    const Real dx = b[0] - a[0], dy = b[1] - a[1], length = dx * dx + dy * dy;
    const Real t =
        length == 0
            ? 0
            : std::clamp(((p[0] - a[0]) * dx + (p[1] - a[1]) * dy) / length, Real(0), Real(1));
    return std::hypot(p[0] - a[0] - t * dx, p[1] - a[1] - t * dy);
}
Real phi(Point p, const Polygon& poly) {
    Real d = std::numeric_limits<Real>::infinity();
    bool inside = false;
    for (std::size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
        const auto a = poly[j], b = poly[i];
        d = std::min(d, distance(p, a, b));
        if ((a[1] > p[1]) != (b[1] > p[1]) &&
            p[0] < (b[0] - a[0]) * (p[1] - a[1]) / (b[1] - a[1]) + a[0])
            inside = !inside;
    }
    Real scale = std::max(std::abs(p[0]), std::abs(p[1]));
    for (auto v : poly)
        scale = std::max({scale, std::abs(v[0]), std::abs(v[1])});
    // Closed polygon boundary, matching tensor-grid containment.
    if (d <= 64 * std::numeric_limits<Real>::epsilon() * scale)
        return 0;
    return inside ? -d : d;
}
// Use the local grid spacing to clamp roundoff at coincident box boundaries.
Real local_h(const HexMeshArrays& mesh, Point p, Point q) {
    Real h = 0;
    for (int axis = 0; axis < 2; ++axis) {
        const auto& x = mesh.axes[axis];
        for (std::size_t i = 1; i < x.size(); ++i)
            if (x[i] >= std::min(p[axis], q[axis]) && x[i - 1] <= std::max(p[axis], q[axis]))
                h = std::max(h, x[i] - x[i - 1]);
    }
    return h;
}
} // namespace
std::vector<BodyLevelSet> region_level_sets(const StackModel& stack,
                                            const HexMeshArrays& mesh,
                                            const RegionLSOptions& options) {
    std::map<BodyId, BodyLevelSet> grouped;
    Real z0 = 0;
    for (const auto& layer : stack.layers) {
        const Real z1 = z0 + layer.thickness;
        for (const auto& r : layer.regions) {
            if (r.footprint.polygon.empty())
                continue;
            const auto name = r.body.empty() ? r.name : r.body;
            auto found = mesh.attr_table.bodies.find(name);
            if (found == mesh.attr_table.bodies.end())
                throw std::invalid_argument("region_level_sets: body '" + name +
                                            "' not in attr_table.bodies");
            auto [it, inserted] = grouped.try_emplace(found->second);
            auto& ls = it->second;
            if (inserted) {
                ls.body = found->second;
                ls.phi.assign(mesh.vertices.size() / 3, std::numeric_limits<Real>::infinity());
            }
            for (std::size_t n = 0; n < ls.phi.size(); ++n) {
                if (mesh.vertices[3 * n + 2] < z0 || mesh.vertices[3 * n + 2] > z1)
                    continue;
                const Point p{mesh.vertices[3 * n], mesh.vertices[3 * n + 1]};
                Real value = phi(p, r.footprint.polygon);
                const Real tol = options.inside_tol_rel * local_h(mesh, p, p);
                if (stack.outline.polygon.empty() &&
                    (std::abs(p[0] - stack.outline.box[0]) <= tol ||
                     std::abs(p[0] - stack.outline.box[2]) <= tol ||
                     std::abs(p[1] - stack.outline.box[1]) <= tol ||
                     std::abs(p[1] - stack.outline.box[3]) <= tol) &&
                    std::abs(value) <= tol)
                    value = -std::max(std::abs(value), std::numeric_limits<Real>::min());
                ls.phi[n] = std::min(ls.phi[n], value);
            }
        }
        z0 = z1;
    }
    std::vector<BodyLevelSet> result;
    for (auto& [id, ls] : grouped)
        result.push_back(std::move(ls));
    return result;
}
} // namespace chipcae::mesh
