#include "chipcae/geometry/ecad/ipc2581.hpp"

#include <algorithm>
#include <clipper2/clipper.h>
#include <cmath>

namespace chipcae::geometry::ecad {
bool rasterize_vias(const EcadModel& model, std::vector<ViaFraction>& cells) {
    for (const auto& c : cells)
        if (c.layer >= model.layers.size() || model.layers[c.layer].copper ||
            !std::isfinite(c.lower[0]) || !std::isfinite(c.lower[1]) ||
            !std::isfinite(c.upper[0]) || !std::isfinite(c.upper[1]) || c.upper[0] <= c.lower[0] ||
            c.upper[1] <= c.lower[1])
            return false;
    for (auto& c : cells) {
        Clipper2Lib::PathsD paths;
        c.via_count = 0;
        for (const auto& v : model.vias) {
            if (std::find(v.dielectric_layers.begin(), v.dielectric_layers.end(), c.layer) ==
                v.dielectric_layers.end())
                continue;
            if (v.position[0] + v.outer_radius_dbu <= c.lower[0] ||
                v.position[0] - v.outer_radius_dbu >= c.upper[0] ||
                v.position[1] + v.outer_radius_dbu <= c.lower[1] ||
                v.position[1] - v.outer_radius_dbu >= c.upper[1])
                continue;
            ++c.via_count;
            // Inscribed regular circles: relative area error < 4e-7, independent of radius.
            // Opposite winding preserves annular holes under NonZero union.
            auto ring = [&](double r, bool reverse) {
                if (r <= 0)
                    return;
                Clipper2Lib::PathD p;
                constexpr int count = 4096;
                p.reserve(count);
                for (int i = 0; i < count; ++i) {
                    const double t = (reverse ? -1 : 1) * 2 * std::acos(-1.0) * i / count;
                    p.emplace_back(v.position[0] + r * std::cos(t),
                                   v.position[1] + r * std::sin(t));
                }
                paths.push_back(std::move(p));
            };
            ring(v.outer_radius_dbu, false);
            ring(v.inner_radius_dbu, true);
        }
        const auto united = Clipper2Lib::Union(paths, Clipper2Lib::FillRule::NonZero, 8);
        Clipper2Lib::PathsD rectangle{{{c.lower[0], c.lower[1]},
                                       {c.upper[0], c.lower[1]},
                                       {c.upper[0], c.upper[1]},
                                       {c.lower[0], c.upper[1]}}};
        const auto clipped =
            Clipper2Lib::Intersect(united, rectangle, Clipper2Lib::FillRule::NonZero, 8);
        const double area = std::abs(Clipper2Lib::Area(clipped));
        c.fraction =
            std::clamp(area / ((c.upper[0] - c.lower[0]) * (c.upper[1] - c.lower[1])), 0.0, 1.0);
    }
    return true;
}
} // namespace chipcae::geometry::ecad
