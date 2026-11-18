#include "chipcae/mesh/cut/outline_cut.hpp"

#include <algorithm>
#include <cmath>

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
    if (ring.empty() && extra.planes.empty())
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
    auto opt = options;
    opt.export_polyhedra = true;
    opt.keep_void = false;
    auto clipped = build_cut_layer(mesh, geometry, opt);
    if (!clipped)
        return Error{"outline", clipped.error().path + ": " + clipped.error().reason};
    if (mesh.elem_gid.empty())
        return Error{"outline", "empty cut domain"};
    return std::optional<CutLayerArrays>{std::move(clipped.value())};
}
} // namespace chipcae::mesh::cut
