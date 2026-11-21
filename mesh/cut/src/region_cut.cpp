#include "chipcae/mesh/cut/region_cut.hpp"

#include "chipcae/mesh/background/tensor_grid.hpp"
#include "chipcae/mesh/cut/detail/convex_pieces.hpp"

#include <algorithm>
#include <cmath>
#include <map>

namespace chipcae::mesh::cut {
namespace {
using Point = std::array<Real, 2>;
using Polygon = std::vector<Point>;

Real edge_length(const Point& a, const Point& b) {
    return std::hypot(b[0] - a[0], b[1] - a[1]);
}

void erase_vertex(Polygon& ring, std::size_t index) {
    ring.erase(ring.begin() + static_cast<Polygon::difference_type>(index));
}

void clean_edges(Polygon& ring) {
    // Choose removals by coordinates, independent of winding and start vertex.
    // For a short edge retain its lexicographically smaller endpoint.
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
            erase_vertex(ring, remove);
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
        erase_vertex(ring, remove);
    }
}

Expected<Polygon> normalized_polygon(const Inclusion& region) {
    auto ring = region.footprint.polygon;
    if (std::any_of(ring.begin(), ring.end(), [](const auto& p) {
            return !std::isfinite(p[0]) || !std::isfinite(p[1]);
        }))
        return Error{"CUT_UNSUPPORTED", region.name + ": degenerate polygon"};
    while (ring.size() > 1 && ring.front() == ring.back())
        ring.pop_back();
    clean_edges(ring);
    Real area = 0;
    // A translated triangle fan avoids cancellation from a common offset.
    for (std::size_t i = 1; i + 1 < ring.size(); ++i)
        area += (ring[i][0] - ring[0][0]) * (ring[i + 1][1] - ring[0][1]) -
                (ring[i][1] - ring[0][1]) * (ring[i + 1][0] - ring[0][0]);
    if (ring.size() < 3 || !std::isfinite(area) || area == 0)
        return Error{"CUT_UNSUPPORTED", region.name + ": degenerate polygon"};
    // Cleanup must not erase a self-touching excursion before topology checks.
    // Repeated closure/consecutive vertices remain accepted normalization input.
    auto topology = region.footprint.polygon;
    topology.erase(std::unique(topology.begin(), topology.end()), topology.end());
    while (topology.size() > 1 && topology.front() == topology.back())
        topology.pop_back();
    if (detail::check_simple(topology) || detail::check_simple(ring))
        return Error{"CUT_UNSUPPORTED", region.name + ": self-intersecting polygon"};
    if (area < 0)
        std::reverse(ring.begin(), ring.end());
    std::rotate(ring.begin(), std::min_element(ring.begin(), ring.end()), ring.end());

    return ring;
}

bool legacy_convex(const Polygon& ring) {
    // Preserve the relative half-space criterion on the unsnapped ring.
    for (std::size_t i = 0; i < ring.size(); ++i) {
        const auto& a = ring[i];
        const auto& b = ring[(i + 1) % ring.size()];
        const Real length = edge_length(a, b);
        const Real nx = (b[1] - a[1]) / length, ny = (a[0] - b[0]) / length;
        const Real d = nx * a[0] + ny * a[1];
        for (const auto& v : ring)
            if (nx * v[0] + ny * v[1] - d > 1e-12 * length)
                return false;
    }
    return true;
}

struct Source {
    std::string body;
    BodyId id;
    Polygon ring;
    std::optional<detail::ConvexPieces> decomposition;
};

Expected<std::vector<Source>> polygon_sources(const StackModel& stack, const HexMeshArrays& mesh) {
    std::vector<Source> sources;
    for (const auto& layer : stack.layers)
        for (const auto& region : layer.regions) {
            if (region.footprint.polygon.empty())
                continue;
            if (!layer.material.empty())
                return Error{"CUT_UNSUPPORTED", region.name + ": polygon in material layer"};
            auto normalized = normalized_polygon(region);
            if (!normalized)
                return normalized.error();
            const auto& body = region.body.empty() ? region.name : region.body;
            const auto found = mesh.attr_table.bodies.find(body);
            if (found == mesh.attr_table.bodies.end())
                return Error{"INPUT_INVALID", body + ": body not in attr_table"};
            auto ring = std::move(normalized.value());
            std::optional<detail::ConvexPieces> decomposition;
            if (!legacy_convex(ring)) {
                auto canonical = detail::normalize_ring(ring);
                if (!canonical)
                    return Error{"CUT_UNSUPPORTED", region.name + ": degenerate polygon"};
                ring = std::move(canonical.value());
                auto pieces = detail::convex_pieces(ring);
                if (!pieces)
                    return Error{"CUT_UNSUPPORTED", region.name + ": " + pieces.error().reason};
                decomposition = std::move(pieces.value());
            }
            sources.push_back({body, found->second, std::move(ring), std::move(decomposition)});
        }
    return sources;
}

Real snapped_coordinate(Real coordinate, const std::vector<Real>& axis) {
    Real nearest = coordinate;
    Real distance = GridOptions{}.snap_tol;
    bool found = false;
    for (const auto line : axis) {
        const Real candidate_distance = std::abs(line - coordinate);
        if (candidate_distance <= distance &&
            (!found || candidate_distance < distance || line < nearest)) {
            nearest = line;
            distance = candidate_distance;
            found = true;
        }
    }
    return nearest;
}

Plane edge_plane(const Point& a, const Point& b, BodyId body, const HexMeshArrays& mesh) {
    const Real dx = b[0] - a[0], dy = b[1] - a[1];
    Plane plane;
    plane.body = body;
    if (dx == 0) {
        plane.n = {dy > 0 ? Real(1) : Real(-1), 0, 0};
        plane.d = plane.n[0] * snapped_coordinate(a[0], mesh.axes[0]);
    } else if (dy == 0) {
        plane.n = {0, dx > 0 ? Real(-1) : Real(1), 0};
        plane.d = plane.n[1] * snapped_coordinate(a[1], mesh.axes[1]);
    } else {
        const Real length = std::hypot(dx, dy);
        plane.n = {dy / length, -dx / length, 0};
        plane.d = plane.n[0] * a[0] + plane.n[1] * a[1];
    }
    return plane;
}
} // namespace

Expected<CutGeometry> region_cut_planes(const StackModel& stack, const HexMeshArrays& mesh) {
    // Validate every source before comparing any same-body rings.
    auto sources = polygon_sources(stack, mesh);
    if (!sources)
        return sources.error();
    std::map<std::string, const Source*> groups;
    for (const auto& source : sources.value()) {
        const auto [it, inserted] = groups.emplace(source.body, &source);
        if (!inserted && it->second->ring != source.ring)
            return Error{"CUT_UNSUPPORTED",
                         source.body + ": body has multiple distinct cut polygons"};
    }
    std::vector<const Source*> ordered;
    for (const auto& group : groups)
        ordered.push_back(group.second);
    std::stable_sort(ordered.begin(), ordered.end(), [](const Source* a, const Source* b) {
        return a->id < b->id;
    });

    CutGeometry geometry;
    for (const auto* source : ordered) {
        if (source->decomposition) {
            PieceUnion target;
            target.body = source->id;
            target.ring = source->ring;
            target.internal = source->decomposition->internal_edge;
            for (const auto& piece : source->decomposition->pieces) {
                std::vector<Plane> planes;
                for (std::size_t i = 0; i < piece.size(); ++i)
                    planes.push_back(
                        edge_plane(piece[i], piece[(i + 1) % piece.size()], source->id, mesh));
                target.pieces.push_back(std::move(planes));
            }
            geometry.unions.push_back(std::move(target));
        } else {
            for (std::size_t i = 0; i < source->ring.size(); ++i)
                geometry.planes.push_back(edge_plane(source->ring[i],
                                                     source->ring[(i + 1) % source->ring.size()],
                                                     source->id,
                                                     mesh));
        }
    }
    return geometry;
}
} // namespace chipcae::mesh::cut
