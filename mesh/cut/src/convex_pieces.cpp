#include "chipcae/mesh/cut/detail/convex_pieces.hpp"

#include "chipcae/mesh/background/tensor_grid.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace chipcae::mesh::cut::detail {
namespace {
Error degenerate() {
    return {"outline.unsupported", "outline must be a convex polygon"};
}

Error nonsimple() {
    return {"outline.unsupported", "self-intersecting outline"};
}

bool finite(Point2 p) {
    return std::isfinite(p[0]) && std::isfinite(p[1]);
}

bool yx_less(const Point2& a, const Point2& b) {
    return a[1] < b[1] || (a[1] == b[1] && a[0] < b[0]);
}

Real edge_length(const Point2& a, const Point2& b) {
    return std::hypot(b[0] - a[0], b[1] - a[1]);
}

// Keep this cleanup identical to region_cut::clean_edges, including its choice
// of removal by coordinates, independent of winding and cyclic start.
void clean_edges(Ring2& ring) {
    while (ring.size() > 1) {
        auto remove = ring.size();
        for (std::size_t i = 0; i < ring.size(); ++i) {
            const auto j = (i + 1) % ring.size();
            if (edge_length(ring[i], ring[j]) <= GridOptions{}.snap_tol) {
                const auto candidate = ring[i] < ring[j] ? j : i;
                if (remove == ring.size() || ring[candidate] < ring[remove])
                    remove = candidate;
            }
        }
        if (remove != ring.size()) {
            ring.erase(ring.begin() + static_cast<Ring2::difference_type>(remove));
            continue;
        }
        for (std::size_t i = 0; i < ring.size(); ++i) {
            const auto& a = ring[(i + ring.size() - 1) % ring.size()];
            const auto& b = ring[i];
            const auto& c = ring[(i + 1) % ring.size()];
            const Real ax = b[0] - a[0], ay = b[1] - a[1];
            const Real bx = c[0] - b[0], by = c[1] - b[1];
            if (std::abs(ax * by - ay * bx) <= 1e-12 * std::hypot(ax, ay) * std::hypot(bx, by)) {
                if (remove == ring.size() || b < ring[remove])
                    remove = i;
            }
        }
        if (remove == ring.size())
            break;
        ring.erase(ring.begin() + static_cast<Ring2::difference_type>(remove));
    }
}

// Topology deliberately does not use either cleanup or half-space tolerances.
// Promote before subtraction to retain the precision of nearby double inputs.
long double orient(const Point2& a, const Point2& b, const Point2& c) {
    const auto ax = static_cast<long double>(b[0]) - a[0];
    const auto ay = static_cast<long double>(b[1]) - a[1];
    const auto bx = static_cast<long double>(c[0]) - a[0];
    const auto by = static_cast<long double>(c[1]) - a[1];
    return ax * by - ay * bx;
}

bool on_segment(const Point2& a, const Point2& b, const Point2& p) {
    return orient(a, b, p) == 0 && p[0] >= std::min(a[0], b[0]) && p[0] <= std::max(a[0], b[0]) &&
           p[1] >= std::min(a[1], b[1]) && p[1] <= std::max(a[1], b[1]);
}

bool opposite(long double a, long double b) {
    return (a < 0 && b > 0) || (a > 0 && b < 0);
}

bool contact(const Point2& a, const Point2& b, const Point2& c, const Point2& d) {
    if (std::max(a[0], b[0]) < std::min(c[0], d[0]) ||
        std::max(c[0], d[0]) < std::min(a[0], b[0]) ||
        std::max(a[1], b[1]) < std::min(c[1], d[1]) || std::max(c[1], d[1]) < std::min(a[1], b[1]))
        return false;
    return (opposite(orient(a, b, c), orient(a, b, d)) &&
            opposite(orient(c, d, a), orient(c, d, b))) ||
           on_segment(a, b, c) || on_segment(a, b, d) || on_segment(c, d, a) || on_segment(c, d, b);
}

bool in_triangle(const Point2& a, const Point2& b, const Point2& c, const Point2& p) {
    return orient(a, b, p) >= 0 && orient(b, c, p) >= 0 && orient(c, a, p) >= 0;
}

struct Halfspace {
    Real nx, ny, d;

    bool outside(const Point2& p) const { return nx * p[0] + ny * p[1] - d > 1e-12; }
};

Halfspace halfspace(const Point2& a, const Point2& b) {
    Real nx = b[1] - a[1], ny = a[0] - b[0];
    const Real length = std::hypot(nx, ny);
    nx /= length;
    ny /= length;
    return {nx, ny, nx * a[0] + ny * a[1]};
}

struct DirectedEdge {
    std::size_t from, to;
};

struct Triangulation {
    std::vector<DirectedEdge> edges;
    std::vector<std::vector<std::size_t>> boundaries; // CCW directed edge IDs
    std::vector<std::pair<std::size_t, std::size_t>> diagonals;
};

Expected<Triangulation> triangulate(const Ring2& ring) {
    const auto n = ring.size();
    Triangulation result;
    std::vector<std::size_t> prev(n), next(n), edge(n), blockers(n);
    std::vector<bool> active(n, true), positive(n);
    for (std::size_t i = 0; i < n; ++i) {
        prev[i] = (i + n - 1) % n;
        next[i] = (i + 1) % n;
        edge[i] = i;
        result.edges.push_back({i, next[i]});
    }

    const auto contains = [&](std::size_t tip, std::size_t v) {
        return in_triangle(ring[prev[tip]], ring[tip], ring[next[tip]], ring[v]);
    };
    const auto recount = [&](std::size_t tip) {
        positive[tip] = orient(ring[prev[tip]], ring[tip], ring[next[tip]]) > 0;
        blockers[tip] = 0;
        if (positive[tip])
            for (std::size_t v = 0; v < n; ++v)
                if (active[v] && v != prev[tip] && v != tip && v != next[tip] && contains(tip, v))
                    ++blockers[tip];
    };
    for (std::size_t i = 0; i < n; ++i)
        recount(i);

    for (auto remaining = n; remaining > 3; --remaining) {
        // IDs never change: the first legal ear has the smallest original ID.
        std::size_t tip = 0;
        while (tip < n && (!active[tip] || !positive[tip] || blockers[tip] != 0))
            ++tip;
        if (tip == n)
            return nonsimple();
        const auto a = prev[tip], b = next[tip];
        const auto forward = result.edges.size(), reverse = forward + 1;
        result.edges.push_back({a, b});
        result.edges.push_back({b, a});
        result.diagonals.emplace_back(forward, reverse);
        result.boundaries.push_back({edge[a], edge[tip], reverse});

        // Unchanged triangles only lose this blocker; only the two neighbors
        // need a complete recount. O(n) work per removal, including boundary hits.
        for (std::size_t v = 0; v < n; ++v)
            if (active[v] && positive[v] && v != tip && v != a && v != b && contains(v, tip))
                --blockers[v];
        active[tip] = false;
        next[a] = b;
        prev[b] = a;
        edge[a] = forward;
        recount(a);
        recount(b);
    }
    const auto a =
        static_cast<std::size_t>(std::find(active.begin(), active.end(), true) - active.begin());
    const auto b = next[a], c = next[b];
    if (orient(ring[a], ring[b], ring[c]) <= 0)
        return nonsimple();
    result.boundaries.push_back({edge[a], edge[b], edge[c]});
    return result;
}

ConvexPieces merge_pieces(const Ring2& ring, Triangulation mesh) {
    const auto n = ring.size(), m = mesh.edges.size(), count = mesh.boundaries.size();
    // All possible surviving directed edges are known after triangulation.
    // Cache exactly the same arithmetic as is_convex, once per edge/vertex pair.
    std::vector<unsigned char> outside(m * n);
    for (std::size_t e = 0; e < m; ++e) {
        const auto h = halfspace(ring[mesh.edges[e].from], ring[mesh.edges[e].to]);
        for (std::size_t v = 0; v < n; ++v)
            outside[e * n + v] = h.outside(ring[v]);
    }
    std::vector<std::vector<std::size_t>> violations(count, std::vector<std::size_t>(m));
    std::vector<std::size_t> owner(m);
    for (std::size_t k = 0; k < count; ++k) {
        for (const auto e : mesh.boundaries[k]) {
            owner[e] = k;
            const auto v = mesh.edges[e].from;
            for (std::size_t h = 0; h < m; ++h)
                violations[k][h] += outside[h * n + v];
        }
    }

    for (const auto& [forward, reverse] : mesh.diagonals) {
        const auto a = owner[forward], b = owner[reverse];
        auto& left = mesh.boundaries[a];
        auto& right = mesh.boundaries[b];
        const auto u = mesh.edges[forward].from, v = mesh.edges[forward].to;
        // The two blocks meet only at this diagonal and its endpoints (the
        // triangulation dual is a tree). Count shared endpoints just once.
        const auto combined = [&](std::size_t e) {
            return violations[a][e] + violations[b][e] - outside[e * n + u] - outside[e * n + v];
        };
        const auto acceptable = [&](const auto& boundary, std::size_t removed) {
            return std::all_of(boundary.begin(), boundary.end(), [&](std::size_t e) {
                return e == removed || combined(e) == 0;
            });
        };
        if (!acceptable(left, forward) || !acceptable(right, reverse))
            continue;

        std::vector<std::size_t> merged;
        merged.reserve(left.size() + right.size() - 2);
        const auto append = [&](const auto& boundary, std::size_t removed) {
            const auto start = static_cast<std::size_t>(
                std::find(boundary.begin(), boundary.end(), removed) - boundary.begin());
            for (std::size_t i = 1; i < boundary.size(); ++i)
                merged.push_back(boundary[(start + i) % boundary.size()]);
        };
        append(left, forward);
        append(right, reverse);
        for (std::size_t e = 0; e < m; ++e)
            violations[a][e] = combined(e);
        for (const auto e : merged)
            owner[e] = a;
        left = std::move(merged);
        right.clear();
    }

    // Rotate cycles without cleaning: collinear subdivision vertices still
    // carry original boundary adjacency and opposite internal-edge pairings.
    std::vector<std::size_t> order;
    for (std::size_t k = 0; k < count; ++k) {
        auto& boundary = mesh.boundaries[k];
        if (boundary.empty())
            continue;
        const auto first = std::min_element(boundary.begin(), boundary.end(), [&](auto a, auto b) {
            return yx_less(ring[mesh.edges[a].from], ring[mesh.edges[b].from]);
        });
        std::rotate(boundary.begin(), first, boundary.end());
        order.push_back(k);
    }
    std::sort(order.begin(), order.end(), [&](auto a, auto b) {
        const auto& left = mesh.boundaries[a];
        const auto& right = mesh.boundaries[b];
        return std::lexicographical_compare(
            left.begin(), left.end(), right.begin(), right.end(), [&](auto u, auto v) {
                return yx_less(ring[mesh.edges[u].from], ring[mesh.edges[v].from]);
            });
    });
    ConvexPieces result;
    for (const auto k : order) {
        Ring2 piece;
        std::vector<bool> internal;
        for (const auto e : mesh.boundaries[k]) {
            const auto [a, b] = mesh.edges[e];
            piece.push_back(ring[a]);
            internal.push_back((a + 1) % n != b && (b + 1) % n != a);
        }
        result.pieces.push_back(std::move(piece));
        result.internal_edge.push_back(std::move(internal));
    }
    return result;
}
} // namespace

Expected<Ring2> normalize_ring(const Ring2& input) {
    if (!std::all_of(input.begin(), input.end(), finite))
        return degenerate();
    auto ring = input;
    while (ring.size() > 1 && ring.front() == ring.back())
        ring.pop_back();
    clean_edges(ring);
    Real area = 0;
    // A translated fan avoids cancellation due to a common coordinate offset.
    for (std::size_t i = 1; i + 1 < ring.size(); ++i)
        area += (ring[i][0] - ring[0][0]) * (ring[i + 1][1] - ring[0][1]) -
                (ring[i][1] - ring[0][1]) * (ring[i + 1][0] - ring[0][0]);
    if (ring.size() < 3 || !std::isfinite(area) || area == 0)
        return degenerate();
    if (area < 0)
        std::reverse(ring.begin(), ring.end());
    std::rotate(ring.begin(), std::min_element(ring.begin(), ring.end(), yx_less), ring.end());
    return ring;
}

bool is_convex(const Ring2& ccw) {
    if (ccw.size() < 3 || !std::all_of(ccw.begin(), ccw.end(), finite))
        return false;
    for (std::size_t i = 0; i < ccw.size(); ++i) {
        const auto& a = ccw[i];
        const auto& b = ccw[(i + 1) % ccw.size()];
        if (edge_length(a, b) == 0)
            return false;
        const auto h = halfspace(a, b);
        for (const auto& v : ccw)
            if (h.outside(v))
                return false;
    }
    return true;
}

std::optional<Error> check_simple(const Ring2& ccw) {
    const auto n = ccw.size();
    if (n < 3 || !std::all_of(ccw.begin(), ccw.end(), finite))
        return nonsimple();
    for (std::size_t i = 0; i < n; ++i) {
        const auto next = (i + 1) % n;
        if (ccw[i] == ccw[next])
            return nonsimple();
        for (std::size_t j = i + 1; j < n; ++j) {
            const auto after = (j + 1) % n;
            if (next == j || after == i) {
                const auto& a = ccw[next == j ? i : j];
                const auto& shared = ccw[next == j ? j : i];
                const auto& b = ccw[next == j ? after : next];
                // Adjacent edges may share the endpoint, but cannot backtrack.
                if (on_segment(a, shared, b) || on_segment(shared, b, a))
                    return nonsimple();
            } else if (contact(ccw[i], ccw[next], ccw[j], ccw[after])) {
                return nonsimple();
            }
        }
    }
    return std::nullopt;
}

bool zero_area_nonsimple(const Ring2& raw) {
    if (!std::all_of(raw.begin(), raw.end(), finite))
        return false;
    auto ring = raw;
    while (ring.size() > 1 && ring.front() == ring.back())
        ring.pop_back();
    clean_edges(ring);
    if (ring.size() < 3)
        return false;
    Real area = 0;
    // A translated fan avoids cancellation due to a common coordinate offset.
    for (std::size_t i = 1; i + 1 < ring.size(); ++i)
        area += (ring[i][0] - ring[0][0]) * (ring[i + 1][1] - ring[0][1]) -
                (ring[i][1] - ring[0][1]) * (ring[i + 1][0] - ring[0][0]);
    if (!std::isfinite(area) || area != 0)
        return false;
    auto topology = raw;
    topology.erase(std::unique(topology.begin(), topology.end()), topology.end());
    while (topology.size() > 1 && topology.front() == topology.back())
        topology.pop_back();
    return check_simple(topology).has_value();
}

Expected<ConvexPieces> convex_pieces(const Ring2& ccw) {
    if (ccw.size() < 3 || !std::all_of(ccw.begin(), ccw.end(), finite))
        return degenerate();
    if (is_convex(ccw))
        return ConvexPieces{{ccw}, {std::vector<bool>(ccw.size(), false)}};
    auto triangles = triangulate(ccw);
    if (!triangles)
        return triangles.error();
    return merge_pieces(ccw, std::move(triangles.value()));
}

Real signed_distance(const Ring2& ccw, Point2 p) {
    if (ccw.empty() || !finite(p) || !std::all_of(ccw.begin(), ccw.end(), finite))
        return std::numeric_limits<Real>::quiet_NaN();
    long double distance = std::numeric_limits<long double>::infinity();
    bool inside = false;
    for (std::size_t i = 0; i < ccw.size(); ++i) {
        const auto& a = ccw[i];
        const auto& b = ccw[(i + 1) % ccw.size()];
        if (on_segment(a, b, p))
            return 0;
        const auto dx = static_cast<long double>(b[0]) - a[0];
        const auto dy = static_cast<long double>(b[1]) - a[1];
        const auto px = static_cast<long double>(p[0]) - a[0];
        const auto py = static_cast<long double>(p[1]) - a[1];
        const auto length2 = dx * dx + dy * dy;
        const auto t = length2 > 0 ? std::clamp((px * dx + py * dy) / length2, 0.L, 1.L) : 0.L;
        distance = std::min(distance, std::hypot(px - t * dx, py - t * dy));
        // Half-open crossings count shared ray/vertex hits exactly once.
        if ((a[1] > p[1]) != (b[1] > p[1]) && (orient(a, b, p) > 0) == (b[1] > a[1]))
            inside = !inside;
    }
    return static_cast<Real>(inside ? -distance : distance);
}
} // namespace chipcae::mesh::cut::detail
