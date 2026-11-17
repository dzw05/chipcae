#include "chipcae/mesh/background/copper_map.hpp"

#include "chipcae/mesh/background/column_hierarchy.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>

#ifdef CHIPCAE_ENABLE_ECAD
#include <clipper2/clipper.h>
#endif

namespace chipcae::mesh {
namespace {
void validate_axes(const std::vector<Real>& lines) {
    if (lines.size() < 2 || !std::is_sorted(lines.begin(), lines.end()))
        throw std::invalid_argument("copper map axes must be strictly increasing");
    for (std::size_t i = 0; i < lines.size(); ++i)
        if (!std::isfinite(lines[i]) || (i && !(lines[i] > lines[i - 1])))
            throw std::invalid_argument("copper map axes must be finite and strictly increasing");
}
} // namespace

std::vector<Real> rasterize_copper(const std::vector<Real>& xs,
                                   const std::vector<Real>& ys,
                                   const std::vector<CopperPath>& paths) {
    validate_axes(xs);
    validate_axes(ys);
#ifndef CHIPCAE_ENABLE_ECAD
    (void)paths;
    throw std::runtime_error("polygon copper rasterization requires CHIPCAE_ENABLE_ECAD");
#else
    using namespace Clipper2Lib;
    struct IndexedPath {
        Real min_x, max_x, min_y, max_y;
        PathD path;
    };
    const Real scale_x = 1.0e6 / (xs.back() - xs.front());
    const Real scale_y = 1.0e6 / (ys.back() - ys.front());
    std::vector<IndexedPath> indexed;
    indexed.reserve(paths.size());
    for (const auto& ring : paths) {
        if (ring.size() < 3)
            continue;
        const auto tx = [&](Real x) { return (x - xs.front()) * scale_x; };
        const auto ty = [&](Real y) { return (y - ys.front()) * scale_y; };
        IndexedPath entry{
            tx(ring.front()[0]), tx(ring.front()[0]), ty(ring.front()[1]), ty(ring.front()[1]), {}};
        entry.path.reserve(ring.size());
        for (const auto& point : ring) {
            if (!std::isfinite(point[0]) || !std::isfinite(point[1]))
                throw std::invalid_argument("copper polygon coordinates must be finite");
            entry.min_x = std::min(entry.min_x, tx(point[0]));
            entry.max_x = std::max(entry.max_x, tx(point[0]));
            entry.min_y = std::min(entry.min_y, ty(point[1]));
            entry.max_y = std::max(entry.max_y, ty(point[1]));
            entry.path.emplace_back(tx(point[0]), ty(point[1]));
        }
        indexed.push_back(std::move(entry));
    }
    std::sort(indexed.begin(), indexed.end(), [](const auto& a, const auto& b) {
        return a.min_x < b.min_x;
    });
    std::vector<std::vector<std::size_t>> x_candidates(xs.size() - 1);
    std::set<std::size_t> active;
    std::multimap<Real, std::size_t> by_max_x;
    std::size_t next = 0;
    for (std::size_t i = 0; i + 1 < xs.size(); ++i) {
        const Real left = (xs[i] - xs.front()) * scale_x;
        const Real right = (xs[i + 1] - xs.front()) * scale_x;
        while (next < indexed.size() && indexed[next].min_x < right) {
            active.insert(next);
            by_max_x.emplace(indexed[next].max_x, next);
            ++next;
        }
        while (!by_max_x.empty() && by_max_x.begin()->first <= left) {
            active.erase(by_max_x.begin()->second);
            by_max_x.erase(by_max_x.begin());
        }
        x_candidates[i].assign(active.begin(), active.end());
    }
    std::vector<Real> result((xs.size() - 1) * (ys.size() - 1));
    for (std::size_t j = 0; j + 1 < ys.size(); ++j) {
        for (std::size_t i = 0; i + 1 < xs.size(); ++i) {
            const RectD rect((xs[i] - xs.front()) * scale_x,
                             (ys[j] - ys.front()) * scale_y,
                             (xs[i + 1] - xs.front()) * scale_x,
                             (ys[j + 1] - ys.front()) * scale_y);
            PathsD clipped;
            for (auto candidate : x_candidates[i]) {
                const auto& p = indexed[candidate];
                if (p.max_y <= rect.top || p.min_y >= rect.bottom)
                    continue;
                auto pieces = RectClip(rect, p.path, 8);
                clipped.insert(clipped.end(), pieces.begin(), pieces.end());
            }
            if (!clipped.empty()) {
                const auto united = Union(clipped, FillRule::NonZero, 8);
                const Real area = std::abs(Area(united));
                const Real cell_area =
                    (xs[i + 1] - xs[i]) * scale_x * (ys[j + 1] - ys[j]) * scale_y;
                result[j * (xs.size() - 1) + i] = std::clamp(area / cell_area, Real(0), Real(1));
            }
        }
    }
    std::vector<Real> morton(result.size());
    const auto nx = xs.size() - 1;
    for (std::size_t j = 0; j + 1 < ys.size(); ++j)
        for (std::size_t i = 0; i + 1 < xs.size(); ++i)
            morton[level0_dense(static_cast<std::uint32_t>(nx),
                                static_cast<std::uint32_t>(ys.size() - 1),
                                static_cast<std::uint32_t>(i),
                                static_cast<std::uint32_t>(j))] = result[j * nx + i];
    return morton;
#endif
}

std::vector<Real> aggregate_phi(const std::vector<Real>& fine,
                                const std::vector<Real>& xs,
                                const std::vector<Real>& ys,
                                Local level) {
    validate_axes(xs);
    validate_axes(ys);
    if (level < 0 || level > 30)
        throw std::invalid_argument("copper aggregate level is out of range");
    const auto nx = xs.size() - 1, ny = ys.size() - 1;
    if (fine.size() != nx * ny)
        throw std::invalid_argument("copper fine field does not match axis dimensions");
    const auto scale = std::size_t{1} << level;
    if (nx % scale || ny % scale)
        throw std::invalid_argument("copper axes must be divisible by the aggregate scale");
    const auto cx = nx / scale, cy = ny / scale;
    std::vector<Real> result(cx * cy, 0);
    // Each block's child Morton IDs are contiguous and parent = child >> (2*level).
    std::vector<Real> sums(cx * cy), areas(cx * cy);
    // Fine dense ordering and its parent relationship are the hierarchy contract.
    for (std::size_t j = 0; j < ny; ++j)
        for (std::size_t i = 0; i < nx; ++i) {
            const auto dense = level0_dense(static_cast<std::uint32_t>(nx),
                                            static_cast<std::uint32_t>(ny),
                                            static_cast<std::uint32_t>(i),
                                            static_cast<std::uint32_t>(j));
            const auto parent = dense >> (2 * level);
            if (parent >= result.size())
                throw std::invalid_argument("fine grid does not follow the column hierarchy");
            const Real area = (xs[i + 1] - xs[i]) * (ys[j + 1] - ys[j]);
            sums[parent] += fine[dense] * area;
            areas[parent] += area;
        }
    for (std::size_t p = 0; p < result.size(); ++p)
        result[p] = sums[p] / areas[p];
    return result;
}
} // namespace chipcae::mesh
