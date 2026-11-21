#include "chipcae/mesh/cut/outline_cut.hpp"

#include "chipcae/mesh/cut/detail/convex_pieces.hpp"
#include "chipcae/mesh/cut/detail/outline_nonconvex.hpp"
#include "chipcae/mesh/cut/detail/plane_cut_union.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace chipcae::mesh::cut {
Expected<std::optional<CutLayerArrays>>
apply_outline_cut(HexMeshArrays& mesh, const Footprint& outline, const CutOptions& options) {
    return apply_outline_cut(mesh, outline, options, CutGeometry{});
}
Expected<std::optional<CutLayerArrays>> apply_outline_cut(HexMeshArrays& mesh,
                                                          const Footprint& outline,
                                                          const CutOptions& options,
                                                          const CutGeometry& extra) {
    auto ring = outline.polygon;
    if (ring.empty() && extra.planes.empty() && extra.unions.empty())
        return std::optional<CutLayerArrays>{};
    CutGeometry geometry;
    if (!ring.empty()) {
        for (auto v : ring) {
            const auto& box = outline.box;
            if (!std::isfinite(v[0]) || !std::isfinite(v[1]) || v[0] < box[0] || v[0] > box[2] ||
                v[1] < box[1] || v[1] > box[3])
                return Error{"outline", "outline polygon vertex outside outline.box"};
        }
        if (ring.size() > 1 && ring.front() == ring.back())
            ring.pop_back();
        double area = 0;
        for (std::size_t i = 0; i < ring.size(); ++i) {
            auto a = ring[i], b = ring[(i + 1) % ring.size()];
            area += a[0] * b[1] - a[1] * b[0];
        }
        if (ring.size() < 3 || !std::isfinite(area) || area == 0)
            return Error{"outline.unsupported", "outline must be a convex polygon"};
        // Canonical winding and start keep plane arithmetic and array order identical.
        if (area < 0)
            std::reverse(ring.begin(), ring.end());
        auto first = std::min_element(ring.begin(), ring.end(), [](auto a, auto b) {
            return a[1] < b[1] || (a[1] == b[1] && a[0] < b[0]);
        });
        std::rotate(ring.begin(), first, ring.end());
        for (std::size_t i = 0; i < ring.size(); ++i) {
            auto a = ring[i], b = ring[(i + 1) % ring.size()];
            double nx = b[1] - a[1], ny = a[0] - b[0];
            const double length = std::hypot(nx, ny);
            if (length == 0)
                return Error{"outline.unsupported", "duplicate outline vertex"};
            nx /= length;
            ny /= length;
            const double d = nx * a[0] + ny * a[1];
            for (auto v : ring)
                if (nx * v[0] + ny * v[1] - d > 1e-12)
                    return Error{"outline.unsupported", "non-convex outlines are not supported"};
            for (const auto& [name, body] : mesh.attr_table.bodies)
                geometry.planes.push_back({body, {nx, ny, 0}, d});
        }
    }
    geometry.planes.insert(geometry.planes.end(), extra.planes.begin(), extra.planes.end());
    geometry.unions = extra.unions;
    auto opt = options;
    opt.export_polyhedra = true;
    opt.keep_void = false;
    auto candidate = mesh;
    auto clipped = build_cut_layer(candidate, geometry, opt);
    if (!clipped) {
        if (!extra.unions.empty())
            return clipped.error();
        return Error{"outline", clipped.error().path + ": " + clipped.error().reason};
    }
    if (candidate.elem_gid.empty())
        return Error{"outline", "empty cut domain"};
    mesh = std::move(candidate);
    return std::optional<CutLayerArrays>{std::move(clipped.value())};
}
} // namespace chipcae::mesh::cut

namespace chipcae::mesh::cut::detail {
namespace {
// Normalization has already failed; non-collinearity determines whether to check
// for crossings. Collapsed and collinear inputs keep normalization's error.
bool crossing_candidate(const Ring2& ring) {
    auto distinct = ring;
    std::sort(distinct.begin(), distinct.end());
    distinct.erase(std::unique(distinct.begin(), distinct.end()), distinct.end());
    if (distinct.size() < 4)
        return false;
    const auto& a = distinct[0];
    const auto& b = distinct[1];
    const auto dx = static_cast<long double>(b[0]) - a[0];
    const auto dy = static_cast<long double>(b[1]) - a[1];
    return std::any_of(distinct.begin() + 2, distinct.end(), [&](const auto& p) {
        return dx * (static_cast<long double>(p[1]) - a[1]) -
                   dy * (static_cast<long double>(p[0]) - a[0]) !=
               0;
    });
}
} // namespace

Expected<std::optional<CutLayerArrays>> apply_outline_cut_general(HexMeshArrays& mesh,
                                                                  const Footprint& outline,
                                                                  const CutOptions& options,
                                                                  const CutGeometry& extra) {
    if (outline.polygon.empty() && extra.planes.empty() && extra.unions.empty())
        return std::optional<CutLayerArrays>{};
    const auto legacy = [&](const Footprint& footprint) -> Expected<std::optional<CutLayerArrays>> {
        auto candidate = mesh;
        auto clipped = apply_outline_cut(candidate, footprint, options, extra);
        if (!clipped)
            return clipped.error();
        if (candidate.elem_gid.empty())
            return Error{"outline", "empty cut domain"};
        mesh = std::move(candidate);
        return clipped;
    };
    if (outline.polygon.empty())
        return legacy(outline);

    auto ring = outline.polygon;
    for (auto v : ring) {
        const auto& box = outline.box;
        if (!std::isfinite(v[0]) || !std::isfinite(v[1]) || v[0] < box[0] || v[0] > box[2] ||
            v[1] < box[1] || v[1] > box[3])
            return Error{"outline", "outline polygon vertex outside outline.box"};
    }
    if (ring.size() > 1 && ring.front() == ring.back())
        ring.pop_back();
    // Match legacy eligibility before cleanup, including its shoelace arithmetic.
    // Accepted collinear subdivisions must still generate the original planes.
    Real area = 0;
    for (std::size_t i = 0; i < ring.size(); ++i) {
        auto a = ring[i], b = ring[(i + 1) % ring.size()];
        area += a[0] * b[1] - a[1] * b[0];
    }
    if (ring.size() >= 3 && std::isfinite(area) && area != 0) {
        if (area < 0)
            std::reverse(ring.begin(), ring.end());
        auto first = std::min_element(ring.begin(), ring.end(), [](auto a, auto b) {
            return a[1] < b[1] || (a[1] == b[1] && a[0] < b[0]);
        });
        std::rotate(ring.begin(), first, ring.end());
        if (is_convex(ring))
            return legacy(outline);
    }

    auto normalized = normalize_ring(outline.polygon);
    if (!normalized) {
        if (crossing_candidate(ring)) {
            while (ring.size() > 1 && ring.front() == ring.back())
                ring.pop_back();
            if (auto error = check_simple(ring))
                return *error;
        }
        return normalized.error();
    }
    ring = std::move(normalized.value());
    if (is_convex(ring)) {
        auto cleaned = outline;
        cleaned.polygon = std::move(ring);
        return legacy(cleaned);
    }
    if (auto error = check_simple(ring))
        return *error;
    auto decomposed = convex_pieces(ring);
    if (!decomposed)
        return decomposed.error();

    std::vector<UnionPiece> pieces;
    pieces.reserve(decomposed.value().pieces.size());
    for (std::size_t k = 0; k < decomposed.value().pieces.size(); ++k) {
        const auto& polygon = decomposed.value().pieces[k];
        UnionPiece piece;
        piece.planes.reserve(polygon.size());
        piece.internal = decomposed.value().internal_edge[k];
        for (std::size_t i = 0; i < polygon.size(); ++i) {
            auto a = polygon[i], b = polygon[(i + 1) % polygon.size()];
            Real nx = b[1] - a[1], ny = a[0] - b[0];
            const Real length = std::hypot(nx, ny);
            nx /= length;
            ny /= length;
            const Real d = nx * a[0] + ny * a[1];
            piece.planes.push_back({0, {nx, ny, 0}, d});
        }
        pieces.push_back(std::move(piece));
    }
    std::vector<Real> outline_phi;
    outline_phi.reserve(mesh.vertices.size() / 3);
    for (std::size_t i = 0; i < mesh.vertices.size(); i += 3)
        outline_phi.push_back(signed_distance(ring, {mesh.vertices[i], mesh.vertices[i + 1]}));
    auto opt = options;
    opt.export_polyhedra = true;
    opt.keep_void = false;
    auto candidate = mesh;
    auto clipped = build_cut_layer_union(candidate, pieces, extra, opt, outline_phi);
    if (!clipped) {
        if (!extra.unions.empty() || clipped.error().path == "outline.unsupported")
            return clipped.error();
        return Error{"outline", clipped.error().path + ": " + clipped.error().reason};
    }
    if (candidate.elem_gid.empty())
        return Error{"outline", "empty cut domain"};
    mesh = std::move(candidate);
    return std::optional<CutLayerArrays>{std::move(clipped.value())};
}
} // namespace chipcae::mesh::cut::detail
