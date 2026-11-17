#include "chipcae/mesh/cut/region_level_set.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace chipcae::mesh {
namespace {
using Point = std::array<Real, 2>;
using Polygon = std::vector<Point>;
Real cross(Point a, Point b, Point c) {
    return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
}
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
    return inside ? -d : d;
}
Real gap(const Polygon& a, const Polygon& b, bool containment = true) {
    Real d = std::numeric_limits<Real>::infinity();
    for (std::size_t i = 0; i < a.size(); ++i)
        for (std::size_t j = 0; j < b.size(); ++j) {
            auto p = a[i], q = a[(i + 1) % a.size()], r = b[j], s = b[(j + 1) % b.size()];
            const Real candidate = std::min(
                {distance(p, r, s), distance(q, r, s), distance(r, p, q), distance(s, p, q)});
            if (candidate == 0 ||
                (cross(p, q, r) * cross(p, q, s) < 0 && cross(r, s, p) * cross(r, s, q) < 0))
                return 0;
            d = std::min(d, candidate);
        }
    if (containment && (phi(a.front(), b) <= 0 || phi(b.front(), a) <= 0))
        return 0;
    return d;
}
std::string body(const Inclusion& r) {
    return r.body.empty() ? r.name : r.body;
}
// Cells touched by a shortest connector form the local distance band. Keeping
// all tied connectors covers parallel facing edges on a graded tensor grid.
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
Point projection(Point p, Point a, Point b) {
    const Real dx = b[0] - a[0], dy = b[1] - a[1];
    const Real norm = dx * dx + dy * dy;
    const Real t =
        norm == 0 ? 0
                  : std::clamp(((p[0] - a[0]) * dx + (p[1] - a[1]) * dy) / norm, Real(0), Real(1));
    return {a[0] + t * dx, a[1] + t * dy};
}
Real band_h(const HexMeshArrays& mesh, const Polygon& a, const Polygon& b) {
    Real best = std::numeric_limits<Real>::infinity(), h = 0;
    for (std::size_t i = 0; i < a.size(); ++i)
        for (std::size_t j = 0; j < b.size(); ++j) {
            const auto p = a[i], q = a[(i + 1) % a.size()];
            const auto r = b[j], s = b[(j + 1) % b.size()];
            const std::array<std::pair<Point, Point>, 4> connectors = {{{p, projection(p, r, s)},
                                                                        {q, projection(q, r, s)},
                                                                        {r, projection(r, p, q)},
                                                                        {s, projection(s, p, q)}}};
            Real pair_best = std::numeric_limits<Real>::infinity();
            Point lo{}, hi{};
            for (const auto& [u, v] : connectors) {
                const Real d = std::hypot(u[0] - v[0], u[1] - v[1]);
                if (d < pair_best) {
                    pair_best = d;
                    lo = {std::min(u[0], v[0]), std::min(u[1], v[1])};
                    hi = {std::max(u[0], v[0]), std::max(u[1], v[1])};
                } else if (d == pair_best) {
                    for (int axis = 0; axis < 2; ++axis) {
                        lo[axis] = std::min({lo[axis], u[axis], v[axis]});
                        hi[axis] = std::max({hi[axis], u[axis], v[axis]});
                    }
                }
            }
            if (pair_best < best) {
                best = pair_best;
                h = local_h(mesh, lo, hi);
            } else if (pair_best == best) {
                h = std::max(h, local_h(mesh, lo, hi));
            }
        }
    return h;
}
void unsupported(const std::string& a, const std::string& b) {
    throw std::runtime_error("CUT_UNSUPPORTED: " + a + " and " + b);
}
} // namespace
std::vector<BodyLevelSet> region_level_sets(const StackModel& stack,
                                            const HexMeshArrays& mesh,
                                            const RegionLSOptions& options) {
    std::vector<BodyLevelSet> result;
    Real z0 = 0;
    for (const auto& layer : stack.layers) {
        const Real z1 = z0 + layer.thickness;
        std::vector<const Inclusion*> polygons;
        for (const auto& r : layer.regions)
            if (!r.footprint.polygon.empty())
                polygons.push_back(&r);
        for (std::size_t i = 0; i < polygons.size(); ++i) {
            const auto& r = *polygons[i];
            if (!layer.material.empty())
                unsupported(r.name, layer.name);
            for (std::size_t j = 0; j < i; ++j) {
                const auto& other = *polygons[j];
                if (body(r) != body(other) &&
                    gap(r.footprint.polygon, other.footprint.polygon) <
                        2 * band_h(mesh, r.footprint.polygon, other.footprint.polygon))
                    unsupported(r.name, other.name);
            }
            if (!stack.outline.polygon.empty() &&
                gap(r.footprint.polygon, stack.outline.polygon, false) <
                    2 * band_h(mesh, r.footprint.polygon, stack.outline.polygon))
                unsupported(r.name, "outline");
        }
        std::vector<std::string> names;
        for (const auto& r : layer.regions) {
            const auto name = body(r);
            if (std::none_of(polygons.begin(), polygons.end(), [&](const auto* polygon) {
                    return body(*polygon) == name;
                }))
                continue;
            if (std::find(names.begin(), names.end(), name) != names.end())
                continue;
            names.push_back(name);
            BodyLevelSet ls;
            ls.body = mesh.attr_table.bodies.at(name);
            ls.phi.assign(mesh.vertices.size() / 3, std::numeric_limits<Real>::infinity());
            for (std::size_t n = 0; n < ls.phi.size(); ++n) {
                if (mesh.vertices[3 * n + 2] < z0 || mesh.vertices[3 * n + 2] > z1)
                    continue;
                const Point p{mesh.vertices[3 * n], mesh.vertices[3 * n + 1]};
                for (const auto* region : polygons)
                    if (body(*region) == name)
                        ls.phi[n] = std::min(ls.phi[n], phi(p, region->footprint.polygon));
                // A strictly negative boundary value also works with cut consumers using phi < 0.
                if (stack.outline.polygon.empty() &&
                    (p[0] == stack.outline.box[0] || p[0] == stack.outline.box[2] ||
                     p[1] == stack.outline.box[1] || p[1] == stack.outline.box[3]) &&
                    std::abs(ls.phi[n]) <= options.inside_tol_rel * local_h(mesh, p, p))
                    ls.phi[n] = -std::max(std::abs(ls.phi[n]), std::numeric_limits<Real>::min());
            }
            result.push_back(std::move(ls));
        }
        z0 = z1;
    }
    return result;
}
} // namespace chipcae::mesh
