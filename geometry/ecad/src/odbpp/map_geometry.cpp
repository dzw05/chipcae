#include "map.hpp"

#include <algorithm>
#include <clipper2/clipper.h>
#include <cmath>
#include <cstdint>
#include <map>
#include <numbers>
#include <numeric>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace chipcae::geometry::ecad::odbpp_detail {
namespace {
using namespace detail;
using namespace Clipper2Lib;
constexpr double pi = std::numbers::pi;
bool on_segment(const Point64& a, const Point64& b, const Point64& p) {
    return CrossProductSign(a, b, p) == 0 && p.x >= std::min(a.x, b.x) &&
           p.x <= std::max(a.x, b.x) && p.y >= std::min(a.y, b.y) && p.y <= std::max(a.y, b.y);
}
bool valid_vertices(const Path64& path) {
    for (std::size_t i = 0; i < path.size(); ++i) {
        const auto& a = path[i];
        const auto& b = path[(i + 1) % path.size()];
        const auto& prev = path[(i + path.size() - 1) % path.size()];
        if (a == b || on_segment(prev, a, b) || on_segment(a, b, prev))
            return false;
    }
    return true;
}
struct EdgeBox {
    std::int64_t xmin, xmax, ymin, ymax;
};
// Static centered interval tree with dynamic membership. Each active interval
// lives at the first center it spans, indexed by both ends. Querying one side
// scans only matching intervals; vertically separated edges never need a flat
// active-set scan. Subtree counts also prune empty portions of the tree.
class ActiveEdges {
    using Entry = std::pair<std::int64_t, std::size_t>;
    struct Node {
        std::set<Entry> lower, upper;
        std::size_t count = 0;
    };
    std::vector<std::int64_t> centers;
    std::vector<Node> nodes;

    template <typename Visit>
    bool query(const EdgeBox& box, std::size_t lo, std::size_t hi, Visit& visit) const {
        if (lo == hi)
            return true;
        const auto mid = lo + (hi - lo) / 2;
        const auto& node = nodes[mid];
        if (!node.count)
            return true;
        if (box.ymax < centers[mid]) {
            for (const auto& [y, index] : node.lower) {
                if (y > box.ymax)
                    break;
                if (!visit(index))
                    return false;
            }
            return query(box, lo, mid, visit);
        }
        if (box.ymin > centers[mid]) {
            for (auto it = node.upper.rbegin(); it != node.upper.rend(); ++it) {
                if (it->first < box.ymin)
                    break;
                if (!visit(it->second))
                    return false;
            }
            return query(box, mid + 1, hi, visit);
        }
        for (const auto& [y, index] : node.lower) {
            (void)y;
            if (!visit(index))
                return false;
        }
        return query(box, lo, mid, visit) && query(box, mid + 1, hi, visit);
    }

public:
    explicit ActiveEdges(const std::vector<EdgeBox>& boxes) {
        centers.reserve(boxes.size());
        for (const auto& box : boxes)
            centers.push_back(box.ymin);
        std::sort(centers.begin(), centers.end());
        centers.erase(std::unique(centers.begin(), centers.end()), centers.end());
        nodes.resize(centers.size());
    }
    void update(const EdgeBox& box, std::size_t index, bool insert) {
        std::size_t lo = 0, hi = centers.size();
        while (lo < hi) {
            const auto mid = lo + (hi - lo) / 2;
            auto& node = nodes[mid];
            if (insert)
                ++node.count;
            else
                --node.count;
            if (box.ymax < centers[mid])
                hi = mid;
            else if (box.ymin > centers[mid])
                lo = mid + 1;
            else {
                if (insert) {
                    node.lower.emplace(box.ymin, index);
                    node.upper.emplace(box.ymax, index);
                } else {
                    node.lower.erase({box.ymin, index});
                    node.upper.erase({box.ymax, index});
                }
                return;
            }
        }
    }
    template <typename Visit>
    bool query(const EdgeBox& box, Visit visit) const {
        return query(box, 0, centers.size(), visit);
    }
};
// Enumerate closed bounding-box overlaps, preserving original edge indices and
// adjacency exclusions. Dense overlapping boxes can still yield O(n^2) pairs.
template <typename Visit>
bool edge_candidates(const Path64& path, Visit visit) {
    std::vector<EdgeBox> boxes;
    boxes.reserve(path.size());
    for (std::size_t i = 0; i < path.size(); ++i) {
        const auto& a = path[i];
        const auto& b = path[(i + 1) % path.size()];
        boxes.push_back(
            {std::min(a.x, b.x), std::max(a.x, b.x), std::min(a.y, b.y), std::max(a.y, b.y)});
    }
    std::vector<std::size_t> starts(path.size());
    std::iota(starts.begin(), starts.end(), std::size_t{0});
    auto ends = starts;
    std::sort(starts.begin(), starts.end(), [&](auto i, auto j) {
        return std::tie(boxes[i].xmin, i) < std::tie(boxes[j].xmin, j);
    });
    std::sort(ends.begin(), ends.end(), [&](auto i, auto j) {
        return std::tie(boxes[i].xmax, i) < std::tie(boxes[j].xmax, j);
    });
    ActiveEdges active(boxes);
    std::size_t expired = 0;
    for (auto index : starts) {
        const auto& box = boxes[index];
        // Strict expiration keeps contacts at equal x coordinates eligible.
        while (expired < ends.size() && boxes[ends[expired]].xmax < box.xmin) {
            auto end = ends[expired++];
            active.update(boxes[end], end, false);
        }
        if (!active.query(box, [&](auto other) {
                const auto i = std::min(index, other), j = std::max(index, other);
                return j == i + 1 || (i == 0 && j + 1 == path.size()) || visit(i, j);
            }))
            return false;
        active.update(box, index, true);
    }
    return true;
}
// Validate integer contour and output edges using the original predicates.
bool simple_ring(const Path64& path) {
    if (!valid_vertices(path))
        return false;
    return edge_candidates(path, [&](auto i, auto j) {
        const auto& a = path[i];
        const auto& b = path[(i + 1) % path.size()];
        const auto& c = path[j];
        const auto& d = path[(j + 1) % path.size()];
        const int ab_c = CrossProductSign(a, b, c), ab_d = CrossProductSign(a, b, d);
        const int cd_a = CrossProductSign(c, d, a), cd_b = CrossProductSign(c, d, b);
        if ((ab_c * ab_d < 0 && cd_a * cd_b < 0) || on_segment(a, b, c) || on_segment(a, b, d) ||
            on_segment(c, d, a) || on_segment(c, d, b))
            return false;
        return true;
    });
}

// Exact signed shoelace sum, accumulated as separate positive and negative
// magnitudes. Clipper supplies full-width unsigned products; checked carries
// extend the sum instead of overflowing or imposing a new coordinate limit.
class ExactArea {
    using Words = std::vector<std::uint64_t>;
    Words positive, negative;
    static void add(Words& words, std::size_t index, std::uint64_t value) {
        while (value) {
            if (index == words.size())
                words.push_back(0);
            const auto old = words[index];
            words[index] += value;
            value = words[index++] < old ? 1 : 0;
        }
    }
    void product(std::int64_t a, std::int64_t b, bool subtract) {
        auto magnitude = [](std::int64_t v) {
            const auto u = static_cast<std::uint64_t>(v);
            return v < 0 ? std::uint64_t{0} - u : u;
        };
        const auto p = MultiplyUInt64(magnitude(a), magnitude(b));
        auto& words = ((a < 0) != (b < 0)) != subtract ? negative : positive;
        add(words, 0, p.lo);
        // A zero low word may not have allocated the first limb.
        if (p.hi && words.empty())
            words.push_back(0);
        add(words, 1, p.hi);
    }

public:
    void append(const Path64& path) {
        for (std::size_t i = 0; i < path.size(); ++i) {
            const auto& a = path[i];
            const auto& b = path[(i + 1) % path.size()];
            product(a.x, b.y, false);
            product(b.x, a.y, true);
        }
    }
    Words magnitude() const {
        auto less = [](const Words& a, const Words& b) {
            return a.size() != b.size()
                       ? a.size() < b.size()
                       : std::lexicographical_compare(a.rbegin(), a.rend(), b.rbegin(), b.rend());
        };
        const bool reversed = less(positive, negative);
        auto result = reversed ? negative : positive;
        const auto& sub = reversed ? positive : negative;
        std::uint64_t borrow = 0;
        for (std::size_t i = 0; i < result.size(); ++i) {
            const auto word = i < sub.size() ? sub[i] : 0;
            const auto old = result[i];
            result[i] -= word;
            const bool underflow = old < word;
            const auto next = result[i];
            result[i] -= borrow;
            borrow = underflow || next < borrow ? 1 : 0;
        }
        while (!result.empty() && !result.back())
            result.pop_back();
        return result;
    }
};

bool bridged_ring(const Path64& path) {
    if (!valid_vertices(path))
        return false;
    auto point_less = [](const Point64& a, const Point64& b) {
        return std::tie(a.x, a.y) < std::tie(b.x, b.y);
    };
    auto key = [&](std::size_t i) {
        auto a = path[i], b = path[(i + 1) % path.size()];
        if (point_less(b, a))
            std::swap(a, b);
        return std::tuple{a.x, a.y, b.x, b.y};
    };
    std::vector<std::size_t> edges(path.size());
    std::iota(edges.begin(), edges.end(), std::size_t{0});
    std::sort(edges.begin(), edges.end(), [&](auto i, auto j) {
        return std::tuple{key(i), i} < std::tuple{key(j), j};
    });
    std::vector<bool> bridge(path.size(), false);
    for (std::size_t begin = 0; begin < edges.size();) {
        auto end = begin + 1;
        while (end < edges.size() && key(edges[begin]) == key(edges[end]))
            ++end;
        // Original vertex validation has already excluded adjacent reversals.
        // Pair opposite directions one-to-one in original occurrence order.
        std::vector<std::size_t> forward, reverse;
        for (auto at = begin; at < end; ++at) {
            auto i = edges[at];
            (point_less(path[i], path[(i + 1) % path.size()]) ? forward : reverse).push_back(i);
        }
        for (std::size_t at = 0; at < std::min(forward.size(), reverse.size()); ++at)
            bridge[forward[at]] = bridge[reverse[at]] = true;
        begin = end;
    }
    if (!edge_candidates(path, [&](auto i, auto j) {
            if (bridge[i] && bridge[j])
                return true;
            const auto& a = path[i];
            const auto& b = path[(i + 1) % path.size()];
            const auto& c = path[j];
            const auto& d = path[(j + 1) % path.size()];
            const int ab_c = CrossProductSign(a, b, c), ab_d = CrossProductSign(a, b, d);
            const int cd_a = CrossProductSign(c, d, a), cd_b = CrossProductSign(c, d, b);
            if (ab_c * ab_d < 0 && cd_a * cd_b < 0)
                return false;
            // Shared endpoints are legal; contacts on an edge interior are not.
            if ((c != a && c != b && on_segment(a, b, c)) ||
                (d != a && d != b && on_segment(a, b, d)) ||
                (a != c && a != d && on_segment(c, d, a)) ||
                (b != c && b != d && on_segment(c, d, b)))
                return false;
            // Equal segments have no endpoint in the other's interior but still
            // overlap over positive length (including same-direction duplicates).
            return !((a == c && b == d) || (a == d && b == c));
        }))
        return false;
    ExactArea input, output;
    input.append(path);
    const auto magnitude = input.magnitude();
    if (magnitude.empty())
        return false;
    for (const auto& ring : Union(Paths64{path}, FillRule::NonZero))
        output.append(ring);
    return magnitude == output.magnitude();
}
// Clipper preserves collinear bridge feet and may join touching islands at a
// repeated vertex. Normalize only newly accepted surfaces to the independent
// boundary loops, before the existing layer union and output-ring validation.
Paths64 surface_rings(const Paths64& paths) {
    Paths64 result;
    for (const auto& path : paths) {
        if (path.empty())
            continue;
        std::map<std::pair<std::int64_t, std::int64_t>, std::size_t> positions;
        Path64 pending;
        for (std::size_t i = 0; i <= path.size(); ++i) {
            const auto& point = path[i % path.size()];
            const auto key = std::pair{point.x, point.y};
            const auto [at, inserted] = positions.emplace(key, pending.size());
            if (inserted) {
                pending.push_back(point);
                continue;
            }
            const auto begin = at->second;
            result.push_back(TrimCollinear(Path64(
                pending.begin() + static_cast<Path64::difference_type>(begin), pending.end())));
            for (auto j = begin + 1; j < pending.size(); ++j)
                positions.erase({pending[j].x, pending[j].y});
            pending.resize(begin + 1);
        }
    }
    return result;
}
// Clipper's AEL is a linked list. A comb with many edges on the same Y scanline
// makes even a union of one simple ring quadratic. Estimate peak active edges
// along each axis and choose the less crowded scan direction for that case.
bool transpose_simple_surface(const Path64& path) {
    auto peak = [&](bool x_axis) {
        std::vector<std::pair<std::int64_t, int>> events;
        events.reserve(2 * path.size());
        for (std::size_t i = 0; i < path.size(); ++i) {
            const auto& a = path[i];
            const auto& b = path[(i + 1) % path.size()];
            const auto u = x_axis ? a.x : a.y, v = x_axis ? b.x : b.y;
            events.emplace_back(std::min(u, v), -1);
            events.emplace_back(std::max(u, v), 1);
        }
        std::sort(events.begin(), events.end());
        std::size_t active = 0, maximum = 0;
        for (const auto& [coordinate, end] : events) {
            (void)coordinate;
            if (end < 0)
                maximum = std::max(maximum, ++active);
            else
                --active;
        }
        return maximum;
    };
    // Avoid extra work for small inputs and nearly equal scan directions.
    return path.size() > 4096 && peak(false) / 4 > peak(true) && simple_ring(path);
}
struct Mapper {
    const OdbJob& job;
    EcadModel& model;
    double scale;
    Point64 point(OdbPoint p) const {
        return {std::llround(p.x * scale), std::llround(p.y * scale)};
    }
    void unsupported(const std::string& text) {
        model.report.unsupported.push_back({"IMPORT_UNSUPPORTED_ELEMENT", text, 0});
    }
    void arc(Path64& path, OdbPoint s, OdbPoint e, OdbPoint c, bool cw) const {
        double a = std::atan2(s.y - c.y, s.x - c.x), b = std::atan2(e.y - c.y, e.x - c.x);
        double sweep = b - a;
        if (cw) {
            if (sweep >= 0)
                sweep -= 2 * pi;
        } else if (sweep <= 0)
            sweep += 2 * pi;
        const double radius = std::hypot(s.x - c.x, s.y - c.y) * scale;
        // Match IPC arc discretization in MM, including its endpoint Y-span tolerance.
        // scale converts input units to micrometre dbu; /1000 gives the Y span
        // in mm. IPC computes tolerance in mm, then *1000 converts it to dbu.
        const double tol =
            std::max(1e-6, 0.01 * std::max(1.0, std::abs(e.y - s.y) * scale / 1000)) * 1000;
        const double step =
            radius > tol ? 2 * std::acos(std::clamp(1 - tol / radius, -1.0, 1.0)) : std::abs(sweep);
        const int count =
            std::max(1, static_cast<int>(std::ceil(std::abs(sweep) / std::max(step, 1e-6))));
        for (int i = 1; i < count; ++i) {
            const double t = a + sweep * i / count;
            path.push_back({std::llround(c.x * scale + radius * std::cos(t)),
                            std::llround(c.y * scale + radius * std::sin(t))});
        }
        path.push_back(point(e));
    }
    Path64 contour(const OdbContour& c,
                   bool allow_bridges = false,
                   bool* normalized_surface = nullptr) const {
        Path64 p{point(c.start)};
        auto s = c.start;
        for (const auto& seg : c.segs) {
            if (seg.is_arc)
                arc(p, s, seg.end, seg.center, seg.cw);
            else
                p.push_back(point(seg.end));
            s = seg.end;
        }
        auto original_size = p.size();
        if (p.size() > 1 && p.front() == p.back())
            --original_size;
        if (allow_bridges)
            StripDuplicates(p, true);
        else if (p.size() > 1 && p.front() == p.back())
            p.pop_back();
        auto distinct = p;
        std::sort(distinct.begin(), distinct.end(), [](const auto& a, const auto& b) {
            return std::tie(a.x, a.y) < std::tie(b.x, b.y);
        });
        distinct.erase(std::unique(distinct.begin(), distinct.end()), distinct.end());
        if (distinct.size() < 3 || Area(p) == 0)
            return {};
        const bool simple = simple_ring(p);
        if (!simple && !(allow_bridges && bridged_ring(p)))
            return {};
        if (normalized_surface && (!simple || p.size() != original_size))
            *normalized_surface = true;
        if ((Area(p) > 0) == c.is_hole)
            std::reverse(p.begin(), p.end());
        return p;
    }
    struct Symbol {
        std::string kind;
        double w = 0, h = 0;
    };
    Symbol symbol(const OdbFeatures& f, int index) const {
        if (index < 0 || static_cast<std::size_t>(index) >= f.symbols.size())
            return {};
        auto name = fold_ascii(f.symbols[static_cast<std::size_t>(index)].name);
        Symbol s;
        std::size_t prefix = 0;
        for (const auto& kind : {std::string("round"), std::string("rect"), std::string("oval")})
            if (name.starts_with(kind)) {
                s.kind = kind;
                prefix = kind.size();
                break;
            }
        if (!prefix)
            return {};
        try {
            auto dims = name.substr(prefix);
            auto x = dims.find('x');
            std::size_t used = 0;
            s.w = std::stod(dims, &used);
            if (used != (x == std::string::npos ? dims.size() : x))
                return {};
            s.h = x == std::string::npos ? s.w : std::stod(dims.substr(x + 1), &used);
            if (x != std::string::npos && used != dims.size() - x - 1)
                return {};
            // Standard symbol dimensions are microns in MM and mils in INCH.
            s.w *= scale / 1000;
            s.h *= scale / 1000;
            if (!(s.w > 0 && s.h > 0))
                return {};
            return s;
        } catch (const std::exception&) {
            return {};
        }
    }
    Symbol stroke_symbol(const OdbFeatures& f, int index) const {
        auto s = symbol(f, index);
        // A stroke has one width: only circular or square apertures are supported.
        if (s.w != s.h)
            return {};
        if (s.kind == "oval")
            s.kind = "round";
        return s;
    }
    Paths64 widen(const Path64& p, const Symbol& s) const {
        ClipperOffset offset;
        // Match IPC: a collapsed ROUND line is a disk with tighter tolerance.
        if (p.size() == 2 && p.front() == p.back())
            offset.ArcTolerance(0.01);
        offset.AddPath(p,
                       s.kind == "rect" ? JoinType::Miter : JoinType::Round,
                       s.kind == "rect" ? EndType::Square : EndType::Round);
        Paths64 out;
        offset.Execute(s.w / 2, out);
        return out;
    }
    Paths64 pad(const OdbPad& p, const Symbol& s) const {
        Path64 local;
        if (s.kind == "rect")
            local = {{std::llround(-s.w / 2), std::llround(-s.h / 2)},
                     {std::llround(s.w / 2), std::llround(-s.h / 2)},
                     {std::llround(s.w / 2), std::llround(s.h / 2)},
                     {std::llround(-s.w / 2), std::llround(s.h / 2)}};
        else if (s.kind == "oval") {
            const double r = std::min(s.w, s.h) / 2;
            const double offset = (std::max(s.w, s.h) - 2 * r) / 2;
            // Include both tangency points of each semicircle so the connecting
            // edges are the capsule's straight sides.
            for (int end = 0; end < 2; ++end)
                for (int i = 0; i <= 256; ++i) {
                    const double a = -pi / 2 + end * pi + pi * i / 256;
                    const double x = (end == 0 ? offset : -offset) + r * std::cos(a);
                    const double y = r * std::sin(a);
                    local.push_back(s.w >= s.h ? Point64{std::llround(x), std::llround(y)}
                                               : Point64{std::llround(-y), std::llround(x)});
                }
        } else {
            const double r = std::min(s.w, s.h) / 2;
            for (int i = 0; i < 256; ++i) {
                const double a = 2 * pi * i / 256;
                local.push_back({std::llround(r * std::cos(a)), std::llround(r * std::sin(a))});
            }
        }
        // Choose local winding so reflection leaves a positive exterior contour.
        if ((Area(local) > 0) == p.mirror)
            std::reverse(local.begin(), local.end());
        double a = p.rot_deg * pi / 180;
        for (auto& v : local) {
            double x = static_cast<double>(v.x) * (p.mirror ? -1 : 1), y = static_cast<double>(v.y);
            v = {std::llround(p.at.x * scale + x * std::cos(a) - y * std::sin(a)),
                 std::llround(p.at.y * scale + x * std::sin(a) + y * std::cos(a))};
        }
        // Enforce final winding independently of placement and coordinate rounding.
        if (Area(local) < 0)
            std::reverse(local.begin(), local.end());
        return {local};
    }
    void copper(EcadLayer& layer, const OdbFeatures& f) {
        Paths64 positive, negative;
        bool transposed = false;
        auto add = [&](Paths64 paths, bool polarity) {
            auto& dst = polarity ? positive : negative;
            dst.insert(dst.end(), paths.begin(), paths.end());
        };
        for (const auto& surface : f.surfaces) {
            Paths64 paths;
            bool normalized_surface = false;
            for (const auto& c : surface.contours) {
                auto path = contour(c, true, &normalized_surface);
                if (path.empty() && !c.segs.empty())
                    throw std::runtime_error("invalid or degenerate contour: " + layer.name);
                if (!path.empty())
                    paths.push_back(std::move(path));
            }
            // With one positive simple contour and no other features, none of
            // these booleans creates intersections. Axis exchange is therefore
            // exact, including the integer output vertices; no rounding changes.
            if (f.surfaces.size() == 1 && surface.positive && paths.size() == 1 && f.pads.empty() &&
                f.lines.empty() && f.arcs.empty() && transpose_simple_surface(paths.front())) {
                transposed = true;
                for (auto& point : paths.front())
                    std::swap(point.x, point.y);
                std::reverse(paths.front().begin(), paths.front().end());
            }
            auto rings = Union(paths, FillRule::NonZero);
            if (normalized_surface)
                rings = surface_rings(rings);
            add(std::move(rings), surface.positive);
        }
        for (const auto& p : f.pads) {
            auto s = symbol(f, p.sym);
            if (s.kind.empty())
                unsupported("Symbol:" + layer.name + ":" + std::to_string(p.sym));
            else
                add(pad(p, s), p.positive);
        }
        for (const auto& l : f.lines) {
            auto s = stroke_symbol(f, l.sym);
            if (s.kind.empty())
                unsupported("Symbol:" + layer.name + ":" + std::to_string(l.sym));
            else
                add(widen({point(l.s), point(l.e)}, s), l.positive);
        }
        for (const auto& a : f.arcs) {
            auto s = stroke_symbol(f, a.sym);
            if (s.kind.empty())
                unsupported("Symbol:" + layer.name + ":" + std::to_string(a.sym));
            else {
                Path64 p{point(a.s)};
                arc(p, a.s, a.e, a.c, a.cw);
                add(widen(p, s), a.positive);
            }
        }
        Clipper64 clipper;
        clipper.AddSubject(Union(positive, FillRule::NonZero));
        clipper.AddClip(Union(negative, FillRule::NonZero));
        PolyTree64 rings;
        if (!clipper.Execute(ClipType::Difference, FillRule::NonZero, rings))
            throw std::runtime_error("copper boolean failed: " + layer.name);
        append_rings(layer, rings);
        if (transposed)
            for (auto& polygon : layer.polygons) {
                for (auto& point : polygon.points)
                    std::swap(point[0], point[1]);
                std::reverse(polygon.points.begin(), polygon.points.end());
            }
    }
    void append_rings(EcadLayer& layer, const PolyPath64& parent) {
        for (const auto& ring : parent) {
            auto path = ring->Polygon();
            StripDuplicates(path, true);
            const auto signed_area = Area(path);
            if (path.size() < 3 || signed_area == 0 || !simple_ring(path))
                throw std::runtime_error("invalid copper output ring: " + layer.name);
            // PolyTree depth defines the required winding independently of area.
            if ((signed_area > 0) == ring->IsHole())
                std::reverse(path.begin(), path.end());
            Polygon p;
            for (const auto& v : path)
                p.points.push_back({v.x, v.y});
            layer.polygons.push_back(std::move(p));
            append_rings(layer, *ring);
        }
    }

    void drill(const OdbLayer& layer, const OdbFeatures& f) {
        for (const auto& l : f.lines) {
            (void)l;
            unsupported("Slot:" + layer.name);
        }
        for (const auto& a : f.arcs) {
            (void)a;
            unsupported("Slot:" + layer.name);
        }
        for (const auto& p : f.pads) {
            if (!p.positive)
                continue;
            auto s = symbol(f, p.sym);
            if (s.kind.empty()) {
                unsupported("Symbol:" + layer.name + ":" + std::to_string(p.sym));
                continue;
            }
            const auto diameter = std::llround(s.w);
            const OdbTool* tool = nullptr;
            auto tools = job.tools.find(layer.name);
            auto classification = [](const OdbTool& t) {
                auto type = fold_ascii(t.type);
                std::erase_if(type, [](char c) { return c == '-' || c == '_'; });
                return type == "plated" ? 0 : type == "slot" ? 1 : type == "nonplated" ? 2 : 3;
            };
            bool ambiguous = false;
            if (tools != job.tools.end())
                for (const auto& t : tools->second)
                    if (std::isfinite(t.finish_size * scale) &&
                        std::llround(t.finish_size * scale) == diameter) {
                        if (tool && classification(*tool) != classification(t))
                            ambiguous = true;
                        else if (!tool)
                            tool = &t;
                    }
            if (ambiguous) {
                unsupported("ViaToolAmbiguous:" + layer.name);
                continue;
            }
            if (s.kind != "round" || (tool && classification(*tool) == 1)) {
                unsupported("Slot:" + layer.name);
                continue;
            }
            if (tool && classification(*tool) == 3) {
                unsupported("ViaToolUnsupported:" + layer.name + ":" + tool->type);
                continue;
            }
            if (tool && classification(*tool) == 2)
                continue;
            auto a = std::find_if(model.layers.begin(), model.layers.end(), [&](const auto& l) {
                return l.name == layer.start_name;
            });
            auto b = std::find_if(model.layers.begin(), model.layers.end(), [&](const auto& l) {
                return l.name == layer.end_name;
            });
            if (a == model.layers.end() || b == model.layers.end() || !a->copper || !b->copper) {
                unsupported("ViaSpan:" + layer.name);
                continue;
            }
            double plating = 0;
            // OdbAttrs::layer represents layers/<name>/attrlist; tools/hits
            // have no attribute storage in the frozen OdbJob contract.
            auto attrs = job.attrs.layer.find(layer.name);
            if (attrs != job.attrs.layer.end()) {
                auto value = attrs->second.find(".plating_thickness");
                if (value != attrs->second.end()) {
                    try {
                        std::size_t used = 0;
                        plating = std::stod(value->second, &used) * scale;
                        if (used != value->second.size())
                            plating = 0;
                    } catch (const std::exception&) {
                        plating = 0;
                    }
                }
            }
            if (!(plating > 0) || !std::isfinite(plating)) {
                unsupported("ViaPlatingMissing:" + layer.name);
                continue;
            }
            Via v;
            v.id = layer.name + ":" + std::to_string(model.vias.size());
            v.start_layer = layer.start_name;
            v.end_layer = layer.end_name;
            auto pos = point(p.at);
            v.position = {pos.x, pos.y};
            v.outer_radius_dbu = static_cast<double>(diameter) / 2;
            v.inner_radius_dbu = v.outer_radius_dbu - plating;
            if (!std::isfinite(v.outer_radius_dbu) || !std::isfinite(v.inner_radius_dbu) ||
                !(0 <= v.inner_radius_dbu && v.inner_radius_dbu < v.outer_radius_dbu)) {
                unsupported("ViaGeometry:" + layer.name);
                continue;
            }
            auto ai = static_cast<std::size_t>(a - model.layers.begin()),
                 bi = static_cast<std::size_t>(b - model.layers.begin());
            for (auto i = std::min(ai, bi) + 1; i < std::max(ai, bi); ++i)
                if (!model.layers[i].copper)
                    v.dielectric_layers.push_back(i);
            model.vias.push_back(std::move(v));
        }
    }
    void fractions() {
        for (std::size_t i = 0; i < model.layers.size(); ++i) {
            ViaFraction cell;
            cell.layer = i;
            bool found = false;
            for (const auto& v : model.vias) {
                if (std::find(v.dielectric_layers.begin(), v.dielectric_layers.end(), i) ==
                    v.dielectric_layers.end())
                    continue;
                for (std::size_t axis = 0; axis < 2; ++axis) {
                    const double lo = v.position[axis] - v.outer_radius_dbu,
                                 hi = v.position[axis] + v.outer_radius_dbu;
                    cell.lower[axis] = found ? std::min(cell.lower[axis], lo) : lo;
                    cell.upper[axis] = found ? std::max(cell.upper[axis], hi) : hi;
                }
                found = true;
            }
            if (found)
                model.via_fractions.push_back(cell);
        }
        if (!rasterize_vias(model, model.via_fractions))
            throw std::runtime_error("invalid via fraction bounds");
    }
};
} // namespace
Expected<EcadModel> map_odb(const OdbJob& job, const MapOptions& options) {
    ImportResult result;
    result.model.id = job.step;
    result.model.dbu_m = 1e-6;
    try {
        const auto units_m = unit_m(job);
        map_stackup(job, options, result);
        Mapper m{job, result.model, units_m / result.model.dbu_m};
        bool exterior = false;
        for (const auto& c : job.profile.contours) {
            if (c.is_hole)
                m.unsupported("ProfileHole");
            else {
                if (exterior)
                    throw std::runtime_error("multiple profile exteriors");
                exterior = true;
                const auto path = m.contour(c);
                if (path.empty())
                    throw std::runtime_error("invalid or degenerate contour");
                for (const auto& p : path)
                    result.model.outline.points.push_back({p.x, p.y});
            }
        }
        for (auto& layer : result.model.layers) {
            auto f = job.features.find(layer.name);
            if (layer.copper && f != job.features.end())
                m.copper(layer, f->second);
        }
        for (const auto& layer : job.layers) {
            auto f = job.features.find(layer.name);
            if (fold_ascii(layer.type) == "drill" && f != job.features.end())
                m.drill(layer, f->second);
        }
        m.fractions();
    } catch (const std::exception& e) {
        return ImportThresholdError{"IMPORT_REJECTED: " + std::string(e.what())};
    }
    return std::move(result.model);
}
} // namespace chipcae::geometry::ecad::odbpp_detail
