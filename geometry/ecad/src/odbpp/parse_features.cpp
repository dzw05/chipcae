#include "parser.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <limits>
#include <map>
#include <numbers>
#include <numeric>
#include <sstream>

namespace chipcae::geometry::ecad::detail {
namespace {
template <typename T>
bool number_value(std::string_view text, T& value) {
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc{} && end == text.data() + text.size() &&
           std::isfinite(static_cast<double>(value));
}
bool exists(const ArchiveReader& reader, const std::string& path) {
    return std::any_of(reader.entries().begin(), reader.entries().end(), [&](const auto& e) {
        return e.path == path && !e.is_dir;
    });
}
bool finite(OdbPoint p) {
    return std::isfinite(p.x) && std::isfinite(p.y);
}
bool distinct(OdbPoint a, OdbPoint b) {
    return a.x != b.x || a.y != b.y;
}
bool arc_geometry(OdbPoint start, OdbPoint end, OdbPoint center) {
    if (!finite(start) || !finite(end) || !finite(center))
        return false;
    const double r = std::hypot(start.x - center.x, start.y - center.y);
    const double q = std::hypot(end.x - center.x, end.y - center.y);
    // Relative tolerance accommodates decimal roundoff without accepting a zero radius.
    return std::isfinite(r) && std::isfinite(q) && r > 0 && q > 0 &&
           std::abs(r - q) <= 1e-9 * std::max(r, q);
}
// Validate the original line/circular-arc geometry without tessellating it.
constexpr double tau = 2 * std::numbers::pi;
double cross(OdbPoint a, OdbPoint b) {
    return a.x * b.y - a.y * b.x;
}
OdbPoint sub(OdbPoint a, OdbPoint b) {
    return {a.x - b.x, a.y - b.y};
}
struct Edge {
    OdbPoint a, b, c;
    bool arc, cw;
    double radius() const { return std::hypot(a.x - c.x, a.y - c.y); }
    double angle() const { return std::atan2(a.y - c.y, a.x - c.x); }
    double sweep() const {
        double d = std::atan2(b.y - c.y, b.x - c.x) - angle();
        if (cw) {
            while (d >= 0)
                d -= tau;
        } else {
            while (d <= 0)
                d += tau;
        }
        return d;
    }
    bool on(OdbPoint p) const {
        if (!arc)
            return std::abs(cross(sub(b, a), sub(p, a))) <=
                       1e-12 * std::hypot(b.x - a.x, b.y - a.y) &&
                   p.x >= std::min(a.x, b.x) - 1e-12 && p.x <= std::max(a.x, b.x) + 1e-12 &&
                   p.y >= std::min(a.y, b.y) - 1e-12 && p.y <= std::max(a.y, b.y) + 1e-12;
        if (std::abs(std::hypot(p.x - c.x, p.y - c.y) - radius()) > 1e-12 * radius())
            return false;
        double d = std::atan2(p.y - c.y, p.x - c.x) - angle();
        if (cw) {
            while (d > 0)
                d -= tau;
        } else {
            while (d < 0)
                d += tau;
        }
        return std::abs(d) <= std::abs(sweep()) + 1e-12;
    }
};
std::vector<Edge> edges(const OdbContour& contour) {
    std::vector<Edge> out;
    auto start = contour.start;
    for (const auto& s : contour.segs) {
        out.push_back({start, s.end, s.center, s.is_arc, s.cw});
        start = s.end;
    }
    // Closure uses the public per-axis tolerance; include its tiny closing edge.
    if (distinct(start, contour.start))
        out.push_back({start, contour.start, {}, false, false});
    return out;
}
std::vector<OdbPoint> intersections(Edge e, Edge f) {
    std::vector<OdbPoint> out;
    auto add = [&](OdbPoint p) {
        if (e.on(p) && f.on(p))
            out.push_back(p);
    };
    if (!e.arc && !f.arc) {
        const auto u = sub(e.b, e.a), v = sub(f.b, f.a);
        const double det = cross(u, v);
        if (det != 0) {
            const double t = cross(sub(f.a, e.a), v) / det;
            add({e.a.x + t * u.x, e.a.y + t * u.y});
        } else {
            for (auto p : {e.a, e.b, f.a, f.b})
                add(p);
        }
    } else if (!e.arc || !f.arc) {
        if (e.arc)
            std::swap(e, f);
        const auto u = sub(e.b, e.a), v = sub(e.a, f.c);
        const double a = cross(u, {-u.y, u.x});
        const double b = 2 * (u.x * v.x + u.y * v.y);
        const double c = v.x * v.x + v.y * v.y - f.radius() * f.radius();
        const double d = b * b - 4 * a * c;
        if (d >= 0 && a > 0)
            for (double t : {(-b - std::sqrt(d)) / (2 * a), (-b + std::sqrt(d)) / (2 * a)})
                add({e.a.x + t * u.x, e.a.y + t * u.y});
    } else {
        const double d = std::hypot(f.c.x - e.c.x, f.c.y - e.c.y);
        const double r = e.radius(), q = f.radius();
        if (d == 0) {
            if (std::abs(r - q) <= 1e-12 * r) {
                for (auto p : {e.a, e.b, f.a, f.b})
                    add(p);
                // Interior samples expose coincident full circles and overlapping arcs.
                for (const auto& g : {e, f}) {
                    const double t = g.angle() + g.sweep() / 2;
                    add({g.c.x + g.radius() * std::cos(t), g.c.y + g.radius() * std::sin(t)});
                }
            }
        } else if (d <= r + q && d >= std::abs(r - q)) {
            const double x = (r * r - q * q + d * d) / (2 * d);
            const double y = std::sqrt(std::max(0.0, r * r - x * x));
            const auto u = sub(f.c, e.c);
            for (double sign : {-1.0, 1.0})
                add({e.c.x + (x * u.x - sign * y * u.y) / d,
                     e.c.y + (x * u.y + sign * y * u.x) / d});
        }
    }
    return out;
}
bool contains(const std::vector<Edge>& ring, OdbPoint p) {
    bool inside = false;
    for (const auto& e : ring) {
        if (!e.arc) {
            if ((e.a.y > p.y) != (e.b.y > p.y) &&
                e.a.x + (p.y - e.a.y) * (e.b.x - e.a.x) / (e.b.y - e.a.y) > p.x)
                inside = !inside;
            continue;
        }
        // Split at vertical extrema, then use the same half-open ray rule as lines.
        const double begin = e.angle(), sweep = e.sweep();
        std::vector<double> cuts{0, 1};
        for (int k = -4; k <= 4; ++k) {
            double t = (std::numbers::pi / 2 + k * std::numbers::pi - begin) / sweep;
            if (t > 0 && t < 1)
                cuts.push_back(t);
        }
        std::sort(cuts.begin(), cuts.end());
        for (std::size_t i = 1; i < cuts.size(); ++i) {
            const double a = begin + cuts[i - 1] * sweep, b = begin + cuts[i] * sweep;
            const double ya = e.c.y + e.radius() * std::sin(a),
                         yb = e.c.y + e.radius() * std::sin(b);
            if ((ya > p.y) == (yb > p.y))
                continue;
            const double dy = p.y - e.c.y;
            const double dx = std::sqrt(std::max(0.0, e.radius() * e.radius() - dy * dy));
            const double x = e.c.x + (std::cos((a + b) / 2) >= 0 ? dx : -dx);
            if (x > p.x)
                inside = !inside;
        }
    }
    return inside;
}
// Bounds only select candidates; the original predicates decide every intersection.
struct Bounds {
    double xmin, ymin, xmax, ymax;
    bool overlaps(const Bounds& b) const {
        return xmin <= b.xmax && b.xmin <= xmax && ymin <= b.ymax && b.ymin <= ymax;
    }
    bool includes(OdbPoint p) const {
        return xmin <= p.x && p.x <= xmax && ymin <= p.y && p.y <= ymax;
    }
    void include(const Bounds& b) {
        xmin = std::min(xmin, b.xmin);
        ymin = std::min(ymin, b.ymin);
        xmax = std::max(xmax, b.xmax);
        ymax = std::max(ymax, b.ymax);
    }
};
constexpr double infinity = std::numeric_limits<double>::infinity();
Bounds bounds(const Edge& e) {
    constexpr double padding = 1e-9;
    const double r = e.arc ? e.radius() : 0;
    const double scale = std::max({std::abs(e.a.x),
                                   std::abs(e.a.y),
                                   std::abs(e.b.x),
                                   std::abs(e.b.y),
                                   e.arc ? std::abs(e.c.x) : 0,
                                   e.arc ? std::abs(e.c.y) : 0,
                                   r});
    // At extreme scales the fixed padding cannot cover radius-relative on() tolerance
    // or arithmetic roundoff (including contains()'s ray calculation). Subnormal
    // line y spans can also underflow in that ray calculation. Keep all pairs
    // in that case, also avoiding overflow in center +/- radius. This is only a
    // conservative broad-phase fallback, not a new geometry tolerance.
    const double dy = std::abs(e.b.y - e.a.y);
    if (!std::isfinite(scale) || scale * std::numeric_limits<double>::epsilon() > padding / 32 ||
        (e.arc && 1e-12 * r > padding / 4) ||
        (!e.arc && dy > 0 && dy < std::numeric_limits<double>::min()))
        return {-infinity, -infinity, infinity, infinity};
    Bounds b = e.arc ? Bounds{e.c.x - r, e.c.y - r, e.c.x + r, e.c.y + r}
                     : Bounds{std::min(e.a.x, e.b.x),
                              std::min(e.a.y, e.b.y),
                              std::max(e.a.x, e.b.x),
                              std::max(e.a.y, e.b.y)};
    // Round outwards even when adding the padding rounds to an existing endpoint.
    return {std::nextafter(b.xmin - padding, -infinity),
            std::nextafter(b.ymin - padding, -infinity),
            std::nextafter(b.xmax + padding, infinity),
            std::nextafter(b.ymax + padding, infinity)};
}
std::vector<Bounds> edge_bounds(const std::vector<Edge>& ring) {
    std::vector<Bounds> boxes;
    boxes.reserve(ring.size());
    for (const auto& e : ring)
        boxes.push_back(bounds(e));
    return boxes;
}
Bounds contour_bounds(const std::vector<Bounds>& boxes) {
    Bounds result{infinity, infinity, -infinity, -infinity};
    for (const auto& box : boxes)
        result.include(box);
    return result;
}
// Fixed (ymin, original index) ranks let the tree prune intervals below the query
// without scanning all x-active edges. Inactive leaves have maximum ymax = -inf.
class ActiveIntervals {
public:
    explicit ActiveIntervals(const std::vector<Bounds>& boxes)
        : boxes_(boxes), order_(boxes.size()) {
        std::iota(order_.begin(), order_.end(), std::size_t{0});
        std::sort(order_.begin(), order_.end(), [&](std::size_t a, std::size_t b) {
            return boxes[a].ymin < boxes[b].ymin || (boxes[a].ymin == boxes[b].ymin && a < b);
        });
        rank_.resize(boxes.size());
        for (std::size_t i = 0; i < order_.size(); ++i)
            rank_[order_[i]] = i;
        while (leaves_ < boxes.size())
            leaves_ *= 2;
        maximum_.assign(2 * leaves_, -infinity);
    }
    void set(std::size_t index, bool active) {
        auto node = leaves_ + rank_[index];
        maximum_[node] = active ? boxes_[index].ymax : -infinity;
        while (node > 1) {
            node /= 2;
            maximum_[node] = std::max(maximum_[2 * node], maximum_[2 * node + 1]);
        }
    }
    template <typename Visit>
    bool query(const Bounds& box, Visit&& visit) const {
        const auto end = std::upper_bound(
            order_.begin(), order_.end(), box.ymax, [&](double y, std::size_t index) {
                return y < boxes_[index].ymin;
            });
        const auto limit = static_cast<std::size_t>(end - order_.begin());
        const auto search =
            [&](auto&& self, std::size_t node, std::size_t left, std::size_t right) -> bool {
            if (left >= limit || maximum_[node] == -infinity || maximum_[node] < box.ymin)
                return true;
            if (right - left == 1)
                return visit(order_[left]);
            const auto middle = left + (right - left) / 2;
            return self(self, 2 * node, left, middle) && self(self, 2 * node + 1, middle, right);
        };
        return search(search, 1, 0, leaves_);
    }

private:
    const std::vector<Bounds>& boxes_;
    std::vector<std::size_t> order_, rank_;
    std::size_t leaves_ = 1;
    std::vector<double> maximum_;
};
// Query before insertion: each unordered pair is visited once. Returning false
// from the callback terminates the sweep immediately. Storage is linear in edges;
// dense box overlap can still require quadratic candidate work.
template <typename Visit>
bool overlapping_pairs(const std::vector<Bounds>& boxes, Visit&& visit) {
    if (boxes.size() < 2)
        return true;
    std::vector<std::size_t> starts(boxes.size()), ends(boxes.size());
    std::iota(starts.begin(), starts.end(), std::size_t{0});
    std::iota(ends.begin(), ends.end(), std::size_t{0});
    std::sort(starts.begin(), starts.end(), [&](std::size_t a, std::size_t b) {
        return boxes[a].xmin < boxes[b].xmin || (boxes[a].xmin == boxes[b].xmin && a < b);
    });
    std::sort(ends.begin(), ends.end(), [&](std::size_t a, std::size_t b) {
        return boxes[a].xmax < boxes[b].xmax || (boxes[a].xmax == boxes[b].xmax && a < b);
    });
    ActiveIntervals active(boxes);
    std::size_t expired = 0;
    for (const auto index : starts) {
        while (expired < ends.size() && boxes[ends[expired]].xmax < boxes[index].xmin)
            active.set(ends[expired++], false);
        if (!active.query(boxes[index], [&](std::size_t other) { return visit(index, other); }))
            return false;
        active.set(index, true);
    }
    return true;
}
bool valid_ring(const OdbContour& contour) {
    const auto ring = edges(contour);
    long double area = 0;
    for (const auto& e : ring) {
        // Translate to the ring origin to avoid cancellation for distant coordinates.
        const auto a = sub(e.a, contour.start), b = sub(e.b, contour.start);
        if (!e.arc)
            area += static_cast<long double>(a.x) * b.y - static_cast<long double>(a.y) * b.x;
        else {
            const auto c = sub(e.c, contour.start);
            area += static_cast<long double>(c.x) * (b.y - a.y) -
                    static_cast<long double>(c.y) * (b.x - a.x) +
                    static_cast<long double>(e.radius()) * e.radius() * e.sweep();
        }
    }
    if (!std::isfinite(area) || area == 0)
        return false;
    return overlapping_pairs(edge_bounds(ring), [&](std::size_t i, std::size_t j) {
        if (j < i)
            std::swap(i, j);
        for (auto p : intersections(ring[i], ring[j])) {
            const auto same = [](OdbPoint a, OdbPoint b) {
                return std::hypot(a.x - b.x, a.y - b.y) <= 1e-12;
            };
            // Consecutive edges, including the synthetic closure, share a vertex.
            if ((j == i + 1 && same(p, ring[i].b)) ||
                (i == 0 && j + 1 == ring.size() && same(p, ring[i].a)))
                continue;
            // ODB exports use retraced bridges to join boundary loops. Shared
            // vertices and collinear bridges are valid; transverse crossings are not.
            if (same(p, ring[i].a) || same(p, ring[i].b) || same(p, ring[j].a) ||
                same(p, ring[j].b))
                continue;
            if (!ring[i].arc && !ring[j].arc &&
                cross(sub(ring[i].b, ring[i].a), sub(ring[j].b, ring[j].a)) == 0)
                continue;
            return false;
        }
        return true;
    });
}

bool valid_topology(const std::vector<OdbContour>& contours) {
    if (contours.empty())
        return false;
    std::vector<std::vector<Edge>> rings;
    std::vector<std::vector<Bounds>> boxes;
    std::vector<Bounds> overall;
    rings.reserve(contours.size());
    boxes.reserve(contours.size());
    overall.reserve(contours.size());
    for (const auto& c : contours) {
        rings.push_back(edges(c));
        boxes.push_back(edge_bounds(rings.back()));
        overall.push_back(contour_bounds(boxes.back()));
    }
    for (std::size_t i = 0; i < rings.size(); ++i) {
        std::size_t parents = 0;
        for (std::size_t j = 0; j < rings.size(); ++j) {
            if (i == j)
                continue;
            if (overall[i].overlaps(overall[j])) {
                auto combined = boxes[i];
                combined.insert(combined.end(), boxes[j].begin(), boxes[j].end());
                const auto split = rings[i].size();
                if (!overlapping_pairs(combined, [&](std::size_t a, std::size_t b) {
                        if (a >= split)
                            std::swap(a, b);
                        if (a >= split || b < split)
                            return true;
                        return intersections(rings[i][a], rings[j][b - split]).empty();
                    }))
                    return false;
            }
            if (overall[j].includes(contours[i].start) && contains(rings[j], contours[i].start)) {
                if (contours[j].is_hole || !contours[i].is_hole)
                    return false;
                ++parents;
            }
        }
        if (parents != (contours[i].is_hole ? 1u : 0u))
            return false;
    }
    return true;
}
// Coordinates stay in job units in the frozen intermediate model.
bool records(const ArchiveReader& reader,
             const std::string& path,
             OdbJob& job,
             OdbFeatures* features,
             ImportResult& result) {
    OdbContour pending_contour;
    OdbSurface pending_surface;
    std::vector<OdbContour> profile_contours;
    OdbContour* contour = nullptr;
    OdbSurface* surface = nullptr;
    bool valid = true;
    std::uint64_t number = 0;
    std::map<int, std::size_t> symbol_ids;
    auto report = [&](const std::string& what) {
        result.model.report.unsupported.push_back(
            {"IMPORT_UNSUPPORTED_ELEMENT", path + ": " + what, number});
    };
    auto fail = [&](const std::string& why) {
        result.error = "IMPORT_REJECTED: " + why;
        valid = false;
        return false;
    };
    auto symbol = [&](int& index) {
        auto found = symbol_ids.find(index);
        if (found == symbol_ids.end()) {
            report("unknown symbol " + std::to_string(index));
            index = -1;
        } else
            index = static_cast<int>(found->second);
    };
    const bool ok = reader.for_each_line(
        path,
        [&](std::string_view raw) {
            ++number;
            // Most large files contain comments; avoid allocating or tokenizing them.
            auto first = raw.find_first_not_of(" \t\r");
            if (first == std::string_view::npos || raw[first] == '#')
                return true;
            auto line = trim(raw.substr(first));
            if (line.starts_with("UNITS=")) {
                auto units = lower(trim(std::string_view(line).substr(6)));
                if ((units != "mm" && units != "inch") ||
                    (units == "inch") != (job.units == OdbUnits::inch))
                    report("unit mismatch");
                return true;
            }
            std::istringstream in(line);
            std::string tag;
            in >> tag;
            if (tag == "F" || tag[0] == '@' || tag[0] == '&' || tag[0] == ';')
                return true;
            if (tag[0] == '$' && features) {
                int index = 0;
                if (!number_value(std::string_view(tag).substr(1), index))
                    return fail("malformed symbol index " + path);
                std::string name;
                in >> name;
                if (index < 0)
                    return fail("invalid symbol index " + path);
                if (symbol_ids.contains(index))
                    return fail("duplicate symbol index");
                symbol_ids.emplace(index, features->symbols.size());
                features->symbols.push_back({name});
                bool known = false;
                for (const auto* prefix : {"r", "oval", "rect", "s"})
                    if (name.starts_with(prefix) &&
                        name.size() > std::char_traits<char>::length(prefix) &&
                        std::isdigit(static_cast<unsigned char>(
                            name[std::char_traits<char>::length(prefix)])))
                        known = true;
                if (!known)
                    report("unsupported symbol " + name);
                return true;
            }
            if (contour && (tag == "S" || tag == "SE" || tag == "OB"))
                return fail("record while contour open " + path);
            if (tag == "S") {
                if (surface)
                    return fail("nested surface " + path);
                char polarity = 'P';
                in >> polarity;
                if (features) {
                    pending_surface = {polarity == 'P', {}};
                    surface = &pending_surface;
                }
            } else if (tag == "OB") {
                OdbContour c;
                char kind = 'I';
                if (!(in >> c.start.x >> c.start.y >> kind))
                    return fail("malformed contour " + path);
                if (!finite(c.start))
                    return fail("non-finite contour " + path);
                c.is_hole = kind == 'H';
                if (features && !surface)
                    return fail("contour outside surface " + path);
                pending_contour = std::move(c);
                contour = &pending_contour;
            } else if (tag == "OS" || tag == "OC") {
                if (!contour)
                    return fail("segment outside contour " + path);
                OdbSegment s;
                if (!(in >> s.end.x >> s.end.y))
                    return fail("malformed segment " + path);
                if (tag == "OC") {
                    char cw = 'N';
                    if (!(in >> s.center.x >> s.center.y >> cw))
                        return fail("malformed arc " + path);
                    s.is_arc = true;
                    s.cw = cw == 'Y';
                }
                const auto start =
                    contour->segs.empty() ? contour->start : contour->segs.back().end;
                if (!finite(s.end) || (s.is_arc && !finite(s.center)))
                    return fail("non-finite segment/arc " + path);
                if (s.is_arc && !arc_geometry(start, s.end, s.center))
                    return fail("invalid arc geometry " + path);
                if (!s.is_arc && !distinct(start, s.end))
                    return true;
                contour->segs.push_back(s);
            } else if (tag == "OE") {
                if (!contour)
                    return fail("contour end without start " + path);
                auto end = contour->segs.empty() ? contour->start : contour->segs.back().end;
                double scale = job.units == OdbUnits::mm ? 0.001 : 0.0254;
                if (std::abs(end.x - contour->start.x) * scale > 1e-9 ||
                    std::abs(end.y - contour->start.y) * scale > 1e-9)
                    return fail(features ? "surface contour not closed"
                                         : "profile contour not closed");
                if (!valid_ring(*contour))
                    return fail("degenerate or self-intersecting contour " + path);
                auto& contours = surface ? surface->contours : profile_contours;
                contours.push_back(std::move(pending_contour));
                contour = nullptr;
            } else if (tag == "SE") {
                if (surface && !valid_topology(surface->contours))
                    return fail("invalid surface topology " + path);
                if (surface)
                    features->surfaces.push_back(std::move(pending_surface));
                surface = nullptr;
            } else if (tag == "P" && features) {
                OdbPad p;
                char polarity = 'P';
                int dcode = 0, orientation = 0;
                if (!(in >> p.at.x >> p.at.y >> p.sym >> polarity >> dcode >> orientation))
                    return fail("malformed pad " + path);
                if (!finite(p.at) || !std::isfinite(p.rot_deg))
                    return fail("non-finite pad " + path);
                p.positive = polarity == 'P';
                if (orientation >= 0 && orientation <= 7) {
                    p.rot_deg = (orientation % 4) * 90;
                    p.mirror = orientation >= 4;
                } else if (orientation == 8 || orientation == 9) {
                    if (!(in >> p.rot_deg))
                        return fail("malformed pad angle " + path);
                    p.mirror = orientation == 9;
                } else
                    report("unknown pad orientation");
                if (!std::isfinite(p.rot_deg))
                    return fail("non-finite pad angle " + path);
                symbol(p.sym);
                features->pads.push_back(p);
            } else if (tag == "L" && features) {
                OdbLine l;
                char polarity = 'P';
                if (!(in >> l.s.x >> l.s.y >> l.e.x >> l.e.y >> l.sym >> polarity))
                    return fail("malformed line " + path);
                if (!finite(l.s) || !finite(l.e) || !distinct(l.s, l.e))
                    return fail("degenerate or non-finite line " + path);
                l.positive = polarity == 'P';
                symbol(l.sym);
                features->lines.push_back(l);
            } else if (tag == "A" && features) {
                OdbArc a;
                char polarity = 'P', cw = 'N';
                int dcode = 0;
                if (!(in >> a.s.x >> a.s.y >> a.e.x >> a.e.y >> a.c.x >> a.c.y >> a.sym >>
                      polarity >> dcode >> cw))
                    return fail("malformed arc " + path);
                if (!arc_geometry(a.s, a.e, a.c))
                    return fail("invalid arc geometry " + path);
                a.positive = polarity == 'P';
                a.cw = cw == 'Y';
                symbol(a.sym);
                features->arcs.push_back(a);
            } else
                report("unsupported record " + tag);
            return true;
        },
        result.error);
    if (!ok || !valid)
        return false;
    if (contour || surface) {
        result.error = "IMPORT_REJECTED: unterminated contour/surface " + path;
        return false;
    }
    if (!features && profile_contours.empty()) {
        result.error = "IMPORT_REJECTED: missing profile contours";
        return false;
    }
    if (!features) {
        if (!valid_topology(profile_contours))
            return fail("invalid profile topology " + path);
        job.profile.contours = std::move(profile_contours);
    }
    return true;
}
} // namespace
bool parse_features(const ArchiveReader& reader, OdbJob& job, ImportResult& result) {
    if (!records(reader, "steps/" + job.step + "/profile", job, nullptr, result))
        return false;
    for (const auto& layer : job.layers) {
        auto base = layer_directory(reader, job, layer.name);
        if (reader.is_supported(base + "features") && exists(reader, base + "features") &&
            !records(reader, base + "features", job, &job.features[layer.name], result))
            return false;
        if (!reader.is_supported(base + "tools") || !exists(reader, base + "tools"))
            continue;
        auto& tools = job.tools[layer.name];
        OdbTool tool;
        bool active = false;
        bool valid = true;
        std::uint64_t number = 0;
        if (!reader.for_each_line(
                base + "tools",
                [&](std::string_view raw) {
                    ++number;
                    auto line = trim(raw);
                    if (line.starts_with("TOOLS") && line.find('{') != std::string::npos) {
                        active = true;
                        tool = {};
                        return true;
                    }
                    if (line == "}" && active) {
                        tools.push_back(tool);
                        active = false;
                        return true;
                    }
                    auto eq = line.find('=');
                    if (eq == std::string::npos)
                        return true;
                    auto key = trim(std::string_view(line).substr(0, eq)),
                         value = trim(std::string_view(line).substr(eq + 1));
                    if (key == "UNITS" &&
                        ((lower(value) == "inch") != (job.units == OdbUnits::inch)))
                        result.model.report.unsupported.push_back(
                            {"IMPORT_UNSUPPORTED_ELEMENT", base + "tools: unit mismatch", number});
                    if (active) {
                        if ((key == "NUM" && !number_value(value, tool.num)) ||
                            (key == "FINISH_SIZE" && !number_value(value, tool.finish_size))) {
                            result.error =
                                "IMPORT_REJECTED: malformed tool " + key + " " + base + "tools";
                            valid = false;
                            return false;
                        }
                        if (key == "TYPE")
                            tool.type = value;
                    }
                    return true;
                },
                result.error))
            return false;
        if (!valid)
            return false;
        if (active) {
            result.error = "IMPORT_REJECTED: unterminated tools " + base + "tools";
            return false;
        }
    }
    return true;
}
} // namespace chipcae::geometry::ecad::detail
