#include "chipcae/mesh/background/copper_map.hpp"

#include "chipcae/mesh/background/column_hierarchy.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
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
#ifdef CHIPCAE_ENABLE_ECAD
// Keep the shared validation order and original integer ring traversal unchanged.
Clipper2Lib::Paths64 native_copper_paths(const PolysetLayer& layer, Real dbu_m) {
    if (!std::isfinite(dbu_m) || !(dbu_m > 0))
        throw std::invalid_argument("copper database unit must be finite and positive");
    if (layer.ring_offsets.size() < 2 || layer.coordinates.size() % 2 != 0)
        throw std::invalid_argument("invalid polyset geometry");
    const auto point_count = layer.coordinates.size() / 2;
    if (layer.ring_offsets.front() != 0 || layer.ring_offsets.back() < 0 ||
        static_cast<std::uint64_t>(layer.ring_offsets.back()) != point_count)
        throw std::invalid_argument("invalid polyset ring offsets");
    // Validate the entire array before constructing any polygon paths.
    for (std::size_t i = 0; i < layer.ring_offsets.size(); ++i) {
        const auto offset = layer.ring_offsets[i];
        if (offset < 0 || static_cast<std::uint64_t>(offset) > point_count ||
            (i > 0 && offset <= layer.ring_offsets[i - 1]))
            throw std::invalid_argument("invalid polyset ring offsets");
    }
    Clipper2Lib::Paths64 paths;
    for (std::size_t r = 0; r + 1 < layer.ring_offsets.size(); ++r) {
        const auto begin = layer.ring_offsets[r], end = layer.ring_offsets[r + 1];
        if (end - begin < 3)
            continue;
        auto& path = paths.emplace_back();
        path.reserve(static_cast<std::size_t>(end - begin));
        for (auto point = static_cast<std::size_t>(begin); point < static_cast<std::size_t>(end);
             ++point)
            path.emplace_back(layer.coordinates[2 * point], layer.coordinates[2 * point + 1]);
    }
    return paths;
}
#endif
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
        std::int64_t min_x, max_x;
        Path64 path;
    };
    const Real scale_x = 1.0e6 / (xs.back() - xs.front());
    const Real scale_y = 1.0e6 / (ys.back() - ys.front());
    // Candidate bounds, both clipping stages and union share a 1e-9
    // normalized-coordinate lattice. A 1e-8 lattice loses too much relative
    // area in narrow graded cells. Quantize each axis line and vertex once.
    constexpr Real lattice = 1.0e9;
    const auto tick = [](Real value) { return std::llround(value * lattice); };
    std::vector<std::int64_t> x_ticks, y_ticks;
    for (auto x : xs)
        x_ticks.push_back(tick((x - xs.front()) * scale_x));
    for (auto y : ys)
        y_ticks.push_back(tick((y - ys.front()) * scale_y));
    std::vector<IndexedPath> indexed;
    indexed.reserve(paths.size());
    for (const auto& ring : paths) {
        if (ring.size() < 3)
            continue;
        const auto tx = [&](Real x) { return (x - xs.front()) * scale_x; };
        const auto ty = [&](Real y) { return (y - ys.front()) * scale_y; };
        IndexedPath entry{std::numeric_limits<std::int64_t>::max(),
                          std::numeric_limits<std::int64_t>::lowest(),
                          {}};
        entry.path.reserve(ring.size());
        for (const auto& point : ring) {
            if (!std::isfinite(point[0]) || !std::isfinite(point[1]))
                throw std::invalid_argument("copper polygon coordinates must be finite");
            const std::int64_t x = tick(tx(point[0])), y = tick(ty(point[1]));
            entry.min_x = std::min(entry.min_x, x);
            entry.max_x = std::max(entry.max_x, x);
            entry.path.emplace_back(x, y);
        }
        indexed.push_back(std::move(entry));
    }
    std::sort(indexed.begin(), indexed.end(), [](const auto& a, const auto& b) {
        return a.min_x < b.min_x;
    });
    std::vector<std::vector<std::size_t>> x_candidates(xs.size() - 1);
    std::set<std::size_t> active;
    std::multimap<std::int64_t, std::size_t> by_max_x;
    std::size_t next = 0;
    for (std::size_t i = 0; i + 1 < xs.size(); ++i) {
        const auto left = x_ticks[i];
        const auto right = x_ticks[i + 1];
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
    struct StripPiece {
        Path64 path;
        std::int64_t min_y, max_y;
    };
    for (std::size_t i = 0; i + 1 < xs.size(); ++i) {
        // Keep strip-generated intersections away from the final cell edges.
        // At least one tick separates each strip edge from its cell edge.
        const auto margin = std::max<std::int64_t>(1, (x_ticks[i + 1] - x_ticks[i]) / 4);
        const Rect64 strip(
            x_ticks[i] - margin, y_ticks.front(), x_ticks[i + 1] + margin, y_ticks.back());
        std::vector<StripPiece> fragments;
        for (auto candidate : x_candidates[i]) {
            for (auto& piece : RectClip(strip, indexed[candidate].path)) {
                auto min_y = piece.front().y, max_y = min_y;
                for (const auto& point : piece) {
                    min_y = std::min(min_y, point.y);
                    max_y = std::max(max_y, point.y);
                }
                fragments.push_back({std::move(piece), min_y, max_y});
            }
        }
        for (std::size_t j = 0; j + 1 < ys.size(); ++j) {
            const Rect64 rect(x_ticks[i], y_ticks[j], x_ticks[i + 1], y_ticks[j + 1]);
            Paths64 clipped;
            for (const auto& fragment : fragments) {
                if (fragment.max_y <= rect.top || fragment.min_y >= rect.bottom)
                    continue;
                auto pieces = RectClip(rect, fragment.path);
                clipped.insert(clipped.end(), pieces.begin(), pieces.end());
            }
            if (!clipped.empty()) {
                auto united = Union(clipped, FillRule::NonZero);
                // Cell-local coordinates reduce cancellation on graded axes.
                // Preserve ring signs, including holes, until after Area sums them.
                for (auto& ring : united)
                    for (auto& point : ring) {
                        point.x -= rect.left;
                        point.y -= rect.top;
                    }
                const Real area = std::abs(Area(united)) / (lattice * lattice);
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
std::vector<Real> copper_column_ref_phi(const std::vector<Real>& xs,
                                        const std::vector<Real>& ys,
                                        const PolysetLayer& layer,
                                        Real dbu_m) {
    validate_axes(xs);
    validate_axes(ys);
#ifndef CHIPCAE_ENABLE_ECAD
    (void)layer;
    (void)dbu_m;
    throw std::runtime_error("polygon copper rasterization requires CHIPCAE_ENABLE_ECAD");
#else
    using namespace Clipper2Lib;
    const auto paths = native_copper_paths(layer, dbu_m);
    const auto nx = xs.size() - 1, ny = ys.size() - 1;
    if (nx > std::numeric_limits<std::uint32_t>::max() ||
        ny > std::numeric_limits<std::uint32_t>::max() ||
        nx > std::numeric_limits<std::size_t>::max() / ny)
        throw std::invalid_argument("copper reference grid dimensions are out of range");
    // Use a conservative subset of Clipper2's coordinate range. Check before
    // Union as well as before converting any floating-point cell boundaries.
    constexpr std::int64_t limit = (std::int64_t{1} << 61) - 1;
    for (const auto& path : paths)
        for (const auto& p : path)
            if (p.x < -limit || p.x > limit || p.y < -limit || p.y > limit)
                throw std::invalid_argument("copper reference coordinates are out of range");
    const auto united = Union(paths, FillRule::NonZero);
    std::vector<Real> result(nx * ny, Real(0));
    const auto native_lines = [&](const std::vector<Real>& lines) {
        std::vector<long double> values;
        values.reserve(lines.size());
        for (const auto line : lines) {
            const long double native = static_cast<long double>(line) / dbu_m;
            if (!std::isfinite(native) || native < -static_cast<long double>(limit) ||
                native > static_cast<long double>(limit))
                throw std::invalid_argument("copper reference coordinates are out of range");
            values.push_back(native);
        }
        return values;
    };
    const auto xt = native_lines(xs), yt = native_lines(ys);
    if (Area(united) == 0)
        return result;
    for (std::size_t j = 0; j < ny; ++j)
        for (std::size_t i = 0; i < nx; ++i) {
            // Clip each native union ring to the four cell half-planes without
            // snapping fractional DBU boundaries to an integer lattice. Signed
            // areas retain holes; bridges between disconnected pieces cancel.
            using Point = std::array<long double, 2>;
            const Point low{xt[i], yt[j]}, high{xt[i + 1], yt[j + 1]};
            long double signed_area = 0;
            for (const auto& ring : united) {
                std::vector<Point> clipped;
                clipped.reserve(ring.size());
                for (const auto& p : ring)
                    clipped.push_back({static_cast<long double>(p.x) - low[0],
                                       static_cast<long double>(p.y) - low[1]});
                for (std::size_t axis = 0; axis < 2; ++axis) {
                    for (const bool upper : {false, true}) {
                        if (clipped.empty())
                            break;
                        const long double edge = upper ? high[axis] - low[axis] : 0;
                        const auto inside = [&](const Point& p) {
                            return upper ? p[axis] <= edge : p[axis] >= edge;
                        };
                        std::vector<Point> output;
                        auto previous = clipped.back();
                        bool previous_inside = inside(previous);
                        for (const auto& current : clipped) {
                            const bool current_inside = inside(current);
                            if (current_inside != previous_inside) {
                                const auto t =
                                    (edge - previous[axis]) / (current[axis] - previous[axis]);
                                Point crossing = previous;
                                crossing[axis] = edge;
                                crossing[1 - axis] += t * (current[1 - axis] - previous[1 - axis]);
                                output.push_back(crossing);
                            }
                            if (current_inside)
                                output.push_back(current);
                            previous = current;
                            previous_inside = current_inside;
                        }
                        clipped = std::move(output);
                    }
                }
                for (std::size_t k = 1; k + 1 < clipped.size(); ++k) {
                    const auto& a = clipped.front();
                    const auto& b = clipped[k];
                    const auto& c = clipped[k + 1];
                    signed_area +=
                        ((b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])) / 2;
                }
            }
            const Real area = static_cast<Real>(std::abs(signed_area) * dbu_m * dbu_m);
            const Real cell_area = (xs[i + 1] - xs[i]) * (ys[j + 1] - ys[j]);
            result[level0_dense(static_cast<std::uint32_t>(nx),
                                static_cast<std::uint32_t>(ny),
                                static_cast<std::uint32_t>(i),
                                static_cast<std::uint32_t>(j))] =
                std::clamp(area / cell_area, Real(0), Real(1));
        }
    return result;
#endif
}

Real copper_union_area_m2(const PolysetLayer& layer, Real dbu_m) {
#ifndef CHIPCAE_ENABLE_ECAD
    (void)layer;
    (void)dbu_m;
    throw std::runtime_error("polygon copper rasterization requires CHIPCAE_ENABLE_ECAD");
#else
    const auto paths = native_copper_paths(layer, dbu_m);
    const auto united = Clipper2Lib::Union(paths, Clipper2Lib::FillRule::NonZero);
    const Real area = Clipper2Lib::Area(united) * dbu_m * dbu_m;
    // Normalize signed zero for JSON output.
    return area == 0 ? Real(0) : area;
#endif
}
} // namespace chipcae::mesh
