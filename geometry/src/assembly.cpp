#include "chipcae/geometry/assembly.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>

namespace chipcae::geometry {
namespace {
using Point = std::array<Real, 2>;
using Polygon = std::vector<Point>;
using chipcae::mesh::Footprint;
using chipcae::mesh::Inclusion;
using chipcae::mesh::Layer;

ImportThresholdError invalid(const std::string& message) {
    return {"INPUT_INVALID: " + message};
}
Real cross(Point a, Point b, Point c) {
    return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
}
Real area(const Polygon& p) {
    Real sum = 0;
    if (p.empty())
        return 0;
    // Translate to avoid cancellation for small shapes far from the origin.
    for (size_t i = 1; i + 1 < p.size(); ++i)
        sum += cross(p[0], p[i], p[i + 1]);
    return std::abs(sum) / 2;
}
bool valid_ring(const Ring& r) {
    return r.pts.size() >= 3 && area(r.pts) > 0 &&
           std::all_of(r.pts.begin(), r.pts.end(), [](Point p) {
               return std::isfinite(p[0]) && std::isfinite(p[1]);
           });
}
// Exact predicates preserve small valid features; normalization handles roundoff later.
bool on_segment(Point a, Point b, Point p) {
    return cross(a, b, p) == 0 && p[0] >= std::min(a[0], b[0]) && p[0] <= std::max(a[0], b[0]) &&
           p[1] >= std::min(a[1], b[1]) && p[1] <= std::max(a[1], b[1]);
}
bool segments_meet(Point a, Point b, Point c, Point d) {
    const Real ac = cross(a, b, c), ad = cross(a, b, d);
    const Real ca = cross(c, d, a), cb = cross(c, d, b);
    return (((ac < 0 && ad > 0) || (ac > 0 && ad < 0)) &&
            ((ca < 0 && cb > 0) || (ca > 0 && cb < 0))) ||
           on_segment(a, b, c) || on_segment(a, b, d) || on_segment(c, d, a) || on_segment(c, d, b);
}
Polygon topology_ring(Polygon p) {
    p.erase(std::unique(p.begin(), p.end()), p.end());
    if (p.size() > 1 && p.front() == p.back())
        p.pop_back();
    return p;
}
bool simple_ring(const Polygon& p) {
    for (size_t i = 0; i < p.size(); ++i) {
        const auto a = p[i], b = p[(i + 1) % p.size()], c = p[(i + 2) % p.size()];
        // Adjacent edges may be collinear, but must not double back.
        if (cross(a, b, c) == 0 && (on_segment(a, b, c) || on_segment(b, c, a)))
            return false;
        for (size_t j = i + 1; j < p.size(); ++j) {
            if (j == i + 1 || (i == 0 && j + 1 == p.size()))
                continue;
            if (segments_meet(a, b, p[j], p[(j + 1) % p.size()]))
                return false;
        }
    }
    return true;
}
bool rings_meet(const Polygon& a, const Polygon& b) {
    for (size_t i = 0; i < a.size(); ++i)
        for (size_t j = 0; j < b.size(); ++j)
            if (segments_meet(a[i], a[(i + 1) % a.size()], b[j], b[(j + 1) % b.size()]))
                return true;
    return false;
}
bool inside_ring(const Polygon& p, Point v) {
    bool inside = false;
    for (size_t i = 0, j = p.size() - 1; i < p.size(); j = i++) {
        const auto a = p[i], b = p[j];
        if ((a[1] > v[1]) != (b[1] > v[1]) &&
            v[0] < a[0] + (v[1] - a[1]) * (b[0] - a[0]) / (b[1] - a[1]))
            inside = !inside;
    }
    return inside;
}
bool valid_topology(const SlabSection& section) {
    const auto outer = topology_ring(section.outer.pts);
    if (!simple_ring(outer))
        return false;
    std::vector<Polygon> holes;
    for (const auto& ring : section.holes) {
        auto hole = topology_ring(ring.pts);
        if (!simple_ring(hole) || rings_meet(outer, hole) || !inside_ring(outer, hole.front()))
            return false;
        for (const auto& prior : holes)
            if (rings_meet(prior, hole) || inside_ring(prior, hole.front()) ||
                inside_ring(hole, prior.front()))
                return false;
        holes.push_back(std::move(hole));
    }
    return true;
}
bool valid_slabs(const std::vector<Slab>& slabs) {
    if (slabs.empty())
        return false;
    for (const auto& s : slabs) {
        if (!std::isfinite(s.z0) || !std::isfinite(s.z1) || s.z0 >= s.z1 || s.sections.empty())
            return false;
        for (const auto& section : s.sections)
            if (!valid_ring(section.outer) ||
                !std::all_of(section.holes.begin(), section.holes.end(), valid_ring) ||
                !valid_topology(section))
                return false;
    }
    return true;
}
Polygon polygon(const Footprint& f) {
    if (!f.polygon.empty())
        return f.polygon;
    return {{f.box[0], f.box[1]}, {f.box[2], f.box[1]}, {f.box[2], f.box[3]}, {f.box[0], f.box[3]}};
}
Footprint footprint(const Polygon& p, bool box_only) {
    Footprint f;
    f.box = {p[0][0], p[0][1], p[0][0], p[0][1]};
    for (Point v : p) {
        f.box[0] = std::min(f.box[0], v[0]);
        f.box[1] = std::min(f.box[1], v[1]);
        f.box[2] = std::max(f.box[2], v[0]);
        f.box[3] = std::max(f.box[3], v[1]);
    }
    if (!box_only)
        f.polygon = p;
    return f;
}
// Restrict coordinate coalescing to floating-point roundoff at this geometry's scale.
Real coordinate_tolerance(const Polygon& p) {
    Real scale = 0;
    for (const auto& v : p)
        scale = std::max({scale, std::abs(v[0]), std::abs(v[1])});
    return 32 * std::numeric_limits<Real>::epsilon() * scale;
}
Polygon normalize(Polygon p) {
    const Real tolerance = coordinate_tolerance(p);
    auto coincident = [=](Point a, Point b) {
        return std::abs(a[0] - b[0]) <= tolerance && std::abs(a[1] - b[1]) <= tolerance;
    };
    p.erase(std::unique(p.begin(), p.end(), coincident), p.end());
    if (p.size() > 1 && coincident(p.front(), p.back()))
        p.pop_back();
    bool changed = true;
    while (changed && p.size() >= 3) {
        changed = false;
        for (size_t i = 0; i < p.size(); ++i) {
            Point a = p[(i + p.size() - 1) % p.size()], b = p[i], c = p[(i + 1) % p.size()];
            Real scale =
                std::hypot(b[0] - a[0], b[1] - a[1]) * std::hypot(c[0] - b[0], c[1] - b[1]);
            if (std::abs(cross(a, b, c)) <= 1e-14 * scale) {
                p.erase(p.begin() + i);
                changed = true;
                break;
            }
        }
    }
    Real signed_area = 0;
    for (size_t i = 1; i + 1 < p.size(); ++i)
        signed_area += cross(p[0], p[i], p[i + 1]);
    if (signed_area < 0)
        std::reverse(p.begin(), p.end());
    if (!p.empty())
        std::rotate(p.begin(), std::min_element(p.begin(), p.end()), p.end());
    return p;
}
bool rectilinear(const Polygon& p) {
    const Real tolerance = coordinate_tolerance(p);
    for (size_t i = 0; i < p.size(); ++i) {
        auto a = p[i], b = p[(i + 1) % p.size()];
        if (std::abs(a[0] - b[0]) > tolerance && std::abs(a[1] - b[1]) > tolerance)
            return false;
    }
    return true;
}
bool same_polygon(const Polygon& a, const Polygon& b) {
    if (a.size() != b.size())
        return false;
    for (size_t shift = 0; shift < b.size(); ++shift) {
        bool equal = true;
        for (size_t i = 0; i < a.size(); ++i)
            for (size_t j = 0; j < 2; ++j)
                equal = equal && std::abs(a[i][j] - b[(i + shift) % b.size()][j]) <= 1e-12;
        if (equal)
            return true;
    }
    return false;
}

// Area queries partition a footprint into horizontal convex trapezoids.
// This helper never constructs output regions or bridges holes.
std::vector<Polygon> fragments(const SlabSection& section) {
    std::vector<const Ring*> rings{&section.outer};
    for (const auto& h : section.holes)
        rings.push_back(&h);
    std::vector<Real> heights;
    for (auto r : rings)
        for (Point p : r->pts)
            heights.push_back(p[1]);
    std::sort(heights.begin(), heights.end());
    heights.erase(std::unique(heights.begin(), heights.end()), heights.end());
    std::vector<Polygon> out;
    for (size_t k = 1; k < heights.size(); ++k) {
        Real lo = heights[k - 1], hi = heights[k], mid = lo + (hi - lo) / 2;
        struct Edge {
            Real x, bottom, top;
        };
        std::vector<Edge> edges;
        for (auto r : rings)
            for (size_t i = 0; i < r->pts.size(); ++i) {
                Point a = r->pts[i], b = r->pts[(i + 1) % r->pts.size()];
                if ((a[1] > mid) == (b[1] > mid))
                    continue;
                auto x = [&](Real y) { return a[0] + (y - a[1]) * (b[0] - a[0]) / (b[1] - a[1]); };
                edges.push_back({x(mid), x(lo), x(hi)});
            }
        std::stable_sort(edges.begin(), edges.end(), [](auto a, auto b) { return a.x < b.x; });
        for (size_t i = 1; i < edges.size(); i += 2) {
            auto l = edges[i - 1], r = edges[i];
            Polygon p{{l.bottom, lo}, {r.bottom, lo}, {r.top, hi}, {l.top, hi}};
            p.erase(std::unique(p.begin(), p.end()), p.end());
            if (p.size() > 1 && p.front() == p.back())
                p.pop_back();
            if (p.size() >= 3 && area(p) > 0)
                out.push_back(std::move(p));
        }
    }
    return out;
}
std::vector<Footprint> footprints(const SlabSection& section, Polygon& prior, std::string& reason) {
    std::vector<Polygon> rings{normalize(section.outer.pts)};
    for (const auto& h : section.holes)
        rings.push_back(normalize(h.pts));
    auto degenerate = [](const Polygon& r) { return r.size() < 3 || !(area(r) > 0); };
    if (std::any_of(rings.begin(), rings.end(), degenerate)) {
        reason = "degenerate section";
        return {};
    }
    bool ortho = std::all_of(rings.begin(), rings.end(), rectilinear);
    if (!ortho) {
        if (rings.size() > 1) {
            reason = "holed non-rectilinear section";
            return {};
        }
        const auto& p = rings.front();
        for (size_t i = 0; i < p.size(); ++i)
            if (cross(p[i], p[(i + 1) % p.size()], p[(i + 2) % p.size()]) <= 0) {
                reason = "non-convex section";
                return {};
            }
        if (!prior.empty() && !same_polygon(prior, p)) {
            reason = "multiple distinct cut sections";
            return {};
        }
        prior = p;
        return {footprint(p, false)};
    }
    // Canonicalize both axes together across outer and hole rings before strip
    // decomposition, so equivalent edges share exact strip endpoints/heights.
    for (size_t axis = 0; axis < 2; ++axis) {
        std::vector<Real> coordinates;
        Real tolerance = 0;
        for (const auto& r : rings) {
            tolerance = std::max(tolerance, coordinate_tolerance(r));
            for (auto p : r)
                coordinates.push_back(p[axis]);
        }
        std::sort(coordinates.begin(), coordinates.end());
        std::vector<Real> canonical;
        for (Real v : coordinates)
            if (canonical.empty() || v - canonical.back() > tolerance)
                canonical.push_back(v);
        for (auto& r : rings)
            for (auto& p : r) {
                auto hi = std::upper_bound(canonical.begin(), canonical.end(), p[axis]);
                p[axis] = *std::prev(hi);
            }
    }
    if (std::any_of(rings.begin(), rings.end(), degenerate)) {
        reason = "degenerate section";
        return {};
    }
    std::vector<Real> xs;
    for (const auto& r : rings)
        for (auto p : r)
            xs.push_back(p[0]);
    std::sort(xs.begin(), xs.end());
    xs.erase(std::unique(xs.begin(), xs.end()), xs.end());
    std::vector<Footprint> out;
    for (size_t k = 1; k < xs.size(); ++k) {
        Real x0 = xs[k - 1], x1 = xs[k], mid = x0 + (x1 - x0) / 2;
        std::vector<Real> ys;
        for (const auto& r : rings)
            for (size_t i = 0; i < r.size(); ++i) {
                auto a = r[i], b = r[(i + 1) % r.size()];
                if ((a[0] > mid) != (b[0] > mid))
                    ys.push_back(a[1]);
            }
        std::sort(ys.begin(), ys.end());
        for (size_t i = 1; i < ys.size(); i += 2) {
            if (ys[i - 1] >= ys[i])
                continue;
            auto found = std::find_if(out.begin(), out.end(), [&](const auto& f) {
                return f.box[2] == x0 && f.box[1] == ys[i - 1] && f.box[3] == ys[i];
            });
            if (found != out.end())
                found->box[2] = x1;
            else {
                Footprint f;
                f.box = {x0, ys[i - 1], x1, ys[i]};
                out.push_back(f);
            }
        }
    }
    if (out.empty())
        reason = "degenerate section";
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.box < b.box; });
    return out;
}
Real convex_intersection(Polygon subject, const Polygon& clip) {
    for (size_t i = 0; i < clip.size() && !subject.empty(); ++i) {
        Point a = clip[i], b = clip[(i + 1) % clip.size()];
        Polygon next;
        Point prev = subject.back();
        Real dp = cross(a, b, prev);
        for (Point cur : subject) {
            Real dc = cross(a, b, cur);
            if ((dp >= 0) != (dc >= 0)) {
                Real t = dp / (dp - dc);
                next.push_back(
                    {prev[0] + t * (cur[0] - prev[0]), prev[1] + t * (cur[1] - prev[1])});
            }
            if (dc >= 0)
                next.push_back(cur);
            prev = cur;
            dp = dc;
        }
        subject = std::move(next);
    }
    return area(subject);
}
Real intersection(const Footprint& a, const Footprint& b) {
    const auto ab = a.polygon.empty() ? a.box : footprint(a.polygon, true).box;
    const auto bbounds = b.polygon.empty() ? b.box : footprint(b.polygon, true).box;
    if (ab[0] >= bbounds[2] || bbounds[0] >= ab[2] || ab[1] >= bbounds[3] || bbounds[1] >= ab[3])
        return 0;
    Real sum = 0;
    auto aa = fragments({Ring{polygon(a)}, {}});
    auto bb = fragments({Ring{polygon(b)}, {}});
    for (const auto& x : aa)
        for (const auto& y : bb)
            sum += convex_intersection(x, y);
    return sum;
}
Defect
diagnostic(Tier tier, DefectReason reason, const std::string& id, const std::string& details) {
    Defect d{};
    d.tier = tier;
    d.reason = reason;
    d.defect_id = id;
    d.details = details;
    return d;
}
Real nearest(Real z, const std::set<Real>& interfaces) {
    auto hi = interfaces.lower_bound(z);
    if (hi == interfaces.begin())
        return *hi;
    if (hi == interfaces.end())
        return *std::prev(hi);
    Real lo = *std::prev(hi);
    return z - lo <= *hi - z ? lo : *hi;
}
} // namespace

Expected<PlacedPart>
place(std::vector<Slab> local, const Placement& placement, const StackModel& base) {
    if (!valid_slabs(local))
        return invalid("empty or invalid local slabs");
    Real dx = 0, dy = 0, dz = 0, angle = 0;
    if (const auto* frame = std::get_if<FramePlacement>(&placement)) {
        dx = frame->origin[0];
        dy = frame->origin[1];
        dz = frame->origin[2];
    } else {
        const auto& attach = std::get<AttachPlacement>(placement);
        if (attach.bond && (!std::isfinite(attach.bond->thickness) || attach.bond->thickness <= 0))
            return invalid("bond thickness must be > 0");
        if (attach.bond && attach.bond->thickness != attach.gap)
            return invalid("bond gap != thickness");
        if (!std::isfinite(attach.gap) || attach.gap < 0)
            return invalid("invalid attachment gap");
        if (attach.target.rfind("part:", 0) == 0 && !attach.resolved_z)
            return invalid("part attach requires resolved_z");
        Real face = 0;
        if (attach.resolved_z) {
            face = *attach.resolved_z;
        } else if (attach.target.rfind("layer:", 0) == 0) {
            bool found = false;
            for (const auto& layer : base.layers) {
                if (!std::isfinite(layer.thickness) || layer.thickness <= 0)
                    return invalid("invalid base layer thickness");
                if (layer.name == attach.target.substr(6)) {
                    if (attach.side == AttachSide::top)
                        face += layer.thickness;
                    found = true;
                    break;
                }
                face += layer.thickness;
            }
            if (!found)
                return invalid("unknown attachment layer");
        } else {
            return invalid("invalid or unresolved attachment target");
        }
        Real lo = local.front().z0, hi = local.front().z1;
        for (const auto& s : local) {
            lo = std::min(lo, s.z0);
            hi = std::max(hi, s.z1);
        }
        dz = attach.side == AttachSide::top ? face + attach.gap - lo : face - attach.gap - hi;
        dx = attach.xy[0];
        dy = attach.xy[1];
        angle = attach.rot_z_deg * (std::acos(Real(-1)) / 180);
    }
    if (!std::isfinite(dx) || !std::isfinite(dy) || !std::isfinite(dz) || !std::isfinite(angle))
        return invalid("nonfinite placement transform");
    const Real c = std::cos(angle), s = std::sin(angle);
    auto transform = [&](Ring& r) {
        for (auto& p : r.pts) {
            Real x = p[0], y = p[1];
            p = {c * x - s * y + dx, s * x + c * y + dy};
        }
    };
    for (auto& slab : local) {
        slab.z0 += dz;
        slab.z1 += dz;
        for (auto& section : slab.sections) {
            transform(section.outer);
            for (auto& h : section.holes)
                transform(h);
        }
    }
    if (!valid_slabs(local))
        return invalid("placement overflow or collapsed geometry");
    return PlacedPart{"", "", std::move(local)};
}

Expected<MergeResult>
merge_parts(StackModel base, const std::vector<PlacedPart>& parts, const MergeOptions& options) {
    if (!std::isfinite(options.t_min) || !std::isfinite(options.eps_weld) ||
        !std::isfinite(options.tau_a) || options.eps_weld <= 0 || options.t_min <= 0 ||
        options.tau_a <= 0)
        return invalid("invalid MergeOptions");
    MergeResult result{base, {}, 0};
    std::set<std::string> ids, layer_names;
    std::vector<Real> boundaries{0};
    for (const auto& l : base.layers) {
        if (!std::isfinite(l.thickness) || l.thickness <= 0 || !layer_names.insert(l.name).second)
            return invalid("invalid base layer thickness or duplicate name");
        boundaries.push_back(boundaries.back() + l.thickness);
    }
    if (parts.empty())
        return result;
    if (base.layers.empty())
        return invalid("base has no layers");
    std::set<std::string> base_names, reserved;
    for (const auto& l : base.layers) {
        base_names.insert(l.name);
        reserved.insert(l.name);
        reserved.insert(l.body.empty() ? l.name : l.body);
        for (const auto& r : l.regions) {
            base_names.insert(r.name);
            reserved.insert(r.name);
            reserved.insert(r.body.empty() ? r.name : r.body);
        }
    }
    for (const auto& p : parts) {
        if (!ids.insert(p.id).second)
            return invalid("duplicate part id " + p.id);
        if (p.id.empty() || !valid_slabs(p.slabs))
            return invalid("empty slabs or invalid geometry");
        for (const auto& name : reserved)
            if (name == p.id || name == p.id + ".bond" || name.starts_with(p.id + "#") ||
                name.starts_with(p.id + ".bond#"))
                return invalid("part id collides with " + name);
    }
    struct Source {
        Real lo, hi;
        std::vector<Footprint> regions;
    };
    std::vector<std::vector<Source>> sources;
    // Resolve support before any source may influence alignment.
    for (const auto& p : parts) {
        std::vector<Source> slabs;
        Polygon prior_polygon;
        for (const auto& s : p.slabs) {
            Source source{s.z0, s.z1, {}};
            std::string reason;
            Polygon candidate_polygon = prior_polygon;
            for (const auto& section : s.sections) {
                auto fs = footprints(section, candidate_polygon, reason);
                source.regions.insert(source.regions.end(), fs.begin(), fs.end());
                if (!reason.empty())
                    break;
            }
            if (!reason.empty()) {
                source.regions.clear();
                result.defects.push_back(
                    diagnostic(Tier::C,
                               DefectReason::PART_NOT_PRISMATIC,
                               p.id + "/cut_unsupported#" + std::to_string(slabs.size()),
                               "cut_unsupported: " + reason));
            } else
                prior_polygon = std::move(candidate_polygon);
            std::stable_sort(source.regions.begin(),
                             source.regions.end(),
                             [](const auto& a, const auto& b) { return a.box < b.box; });
            slabs.push_back(std::move(source));
        }
        sources.push_back(std::move(slabs));
    }
    std::set<Real> interfaces(boundaries.begin(), boundaries.end());
    // Establish all ordinary candidate interfaces in sorted order, independent
    // of part precedence. Base interfaces always have priority over new ones.
    std::set<Real> candidates;
    for (const auto& slabs : sources)
        for (const auto& source : slabs)
            if (!source.regions.empty()) {
                candidates.insert(source.lo);
                candidates.insert(source.hi);
            }
    for (Real z : candidates) {
        Real target = nearest(z, interfaces);
        if (std::abs(z - target) >= options.t_min)
            interfaces.insert(z);
    }
    auto aligned = [&](Real z) {
        Real target = nearest(z, interfaces);
        return std::abs(z - target) < options.t_min ? target : z;
    };
    std::set<Real> snapped_interfaces;
    for (Real z : candidates) {
        Real target = aligned(z);
        if (target != z && std::abs(z - target) < options.eps_weld)
            snapped_interfaces.insert(target);
    }
    result.welded_interfaces = static_cast<std::int64_t>(snapped_interfaces.size());
    // Unsupported slabs contribute neither alignment candidates nor provenance cuts.
    // Retain supported pre-alignment endpoints for stable thin interval numbering.
    std::set<Real> original_interfaces = candidates;
    original_interfaces.insert(boundaries.begin(), boundaries.end());
    for (size_t p_index = 0; p_index < parts.size(); ++p_index) {
        const auto& p = parts[p_index];
        size_t thin_index = 0;
        size_t slab_index = 0;
        bool bond = p.id.size() >= 5 && p.id.compare(p.id.size() - 5, 5, ".bond") == 0;
        for (const auto& s : p.slabs) {
            auto& source = sources[p_index][slab_index++];
            source.lo = aligned(s.z0);
            source.hi = aligned(s.z1);
            // Preserve per-source provenance for each retained interval before
            // alignment, including thin pieces created by a base boundary.
            std::vector<Real> cuts{s.z0};
            // Layer sums and literal slab endpoints can differ by one ulp.
            // Do not turn that arithmetic roundoff into a provenance interval.
            auto coincident = [](Real a, Real b) {
                return std::abs(a - b) <= 4 * std::numeric_limits<Real>::epsilon() *
                                              std::max(std::abs(a), std::abs(b));
            };
            for (Real z : original_interfaces)
                if (z > s.z0 && z < s.z1 && !coincident(z, s.z0) && !coincident(z, s.z1) &&
                    !coincident(z, cuts.back()))
                    cuts.push_back(z);
            cuts.push_back(s.z1);
            auto report_thin = [&](Real before, Real after) {
                std::ostringstream details;
                details << std::setprecision(17) << "original thickness " << before
                        << "; resulting thickness " << after;
                result.defects.push_back(diagnostic(Tier::B,
                                                    DefectReason::THIN_LAYER_MERGED,
                                                    p.id + "/thin#" + std::to_string(thin_index),
                                                    details.str()));
            };
            for (size_t i = 1; i < cuts.size(); ++i) {
                Real t = cuts[i] - cuts[i - 1];
                if (!source.regions.empty() && t < options.t_min &&
                    (bond || t >= options.eps_weld)) {
                    Real after = std::max(Real(0), aligned(cuts[i]) - aligned(cuts[i - 1]));
                    report_thin(t, after);
                }
                ++thin_index;
            }
        }
    }
    // Only surviving geometry may split or extend the base stack. Keep the
    // original Source entries so slab indices in diagnostics and names survive.
    std::set<Real> surviving_interfaces(boundaries.begin(), boundaries.end());
    for (const auto& part_sources : sources)
        for (const auto& source : part_sources)
            if (source.lo < source.hi && !source.regions.empty()) {
                surviving_interfaces.insert(source.lo);
                surviving_interfaces.insert(source.hi);
            }
    std::vector<Real> zs(surviving_interfaces.begin(), surviving_interfaces.end());
    result.stack.layers.clear();
    std::vector<size_t> split_counts(base.layers.size(), 0), split_indices(base.layers.size(), 0);
    auto owner = [&](Real midpoint) -> size_t {
        if (midpoint < 0 || midpoint >= boundaries.back())
            return base.layers.size();
        return std::upper_bound(boundaries.begin(), boundaries.end(), midpoint) -
               boundaries.begin() - 1;
    };
    for (size_t i = 1; i < zs.size(); ++i) {
        size_t o = owner(zs[i - 1] + (zs[i] - zs[i - 1]) / 2);
        if (o < base.layers.size())
            ++split_counts[o];
    }
    std::set<std::string> used_names;
    // Reserve unchanged names first; generated names must never steal them.
    for (size_t i = 0; i < base.layers.size(); ++i)
        if (split_counts[i] == 1)
            used_names.insert(base.layers[i].name);
    std::map<std::string, std::vector<std::string>> layer_pieces, region_pieces;
    std::map<std::string, size_t> layer_order;
    for (size_t o = 0; o < base.layers.size(); ++o) {
        const auto& l = base.layers[o];
        layer_order[l.name] = o;
        for (size_t k = 0; k < split_counts[o]; ++k) {
            auto suffix = split_counts[o] > 1 ? "#" + std::to_string(k) : "";
            for (const auto& name : {l.name + suffix})
                if (!suffix.empty() && base_names.count(name))
                    return invalid("split name collides with " + name);
            layer_pieces[l.name].push_back(l.name + suffix);
            used_names.insert(l.name + suffix);
            for (const auto& r : l.regions) {
                if (!suffix.empty() && base_names.count(r.name + suffix))
                    return invalid("split name collides with " + r.name + suffix);
                region_pieces[r.name].push_back(r.name + suffix);
            }
        }
    }
    for (auto& [group, refs] : result.stack.activation_groups) {
        std::vector<std::string> rewritten;
        for (auto ref : refs) {
            bool expanded = false;
            for (const auto& prefix : {std::string("layer:"), std::string("region:")}) {
                auto& pieces = prefix == "layer:" ? layer_pieces : region_pieces;
                if (ref.starts_with(prefix) && pieces.count(ref.substr(prefix.size()))) {
                    for (const auto& name : pieces.at(ref.substr(prefix.size())))
                        rewritten.push_back(prefix + name);
                    expanded = true;
                    break;
                }
            }
            if (expanded)
                continue;
            if (ref.starts_with("interface:")) {
                auto sep = ref.find('|');
                if (sep != std::string::npos) {
                    auto a = ref.substr(10, sep - 10), b = ref.substr(sep + 1);
                    if (layer_order.count(a) && layer_order.count(b) &&
                        (layer_order[a] + 1 == layer_order[b] ||
                         layer_order[b] + 1 == layer_order[a])) {
                        bool below = layer_order[a] < layer_order[b];
                        ref = "interface:" +
                              (below ? layer_pieces[a].back() : layer_pieces[a].front()) + "|" +
                              (below ? layer_pieces[b].front() : layer_pieces[b].back());
                    }
                }
            }
            rewritten.push_back(ref);
        }
        refs = std::move(rewritten);
    }
    for (size_t i = 1; i < zs.size(); ++i) {
        Real lo = zs[i - 1], hi = zs[i];
        size_t o = owner(lo + (hi - lo) / 2);
        Layer l;
        if (o < base.layers.size()) {
            l = base.layers[o];
            l.body = l.body.empty() ? l.name : l.body;
            auto suffix = split_counts[o] > 1 ? "#" + std::to_string(split_indices[o]++) : "";
            l.name += suffix;
            for (auto& r : l.regions) {
                r.body = r.body.empty() ? r.name : r.body;
                r.name += suffix;
            }
        } else {
            l.name = "assembly#" + std::to_string(i - 1);
            if (base_names.count(l.name) || !used_names.insert(l.name).second)
                return invalid("split name collides with " + l.name);
            l.kind = "Explicit";
        }
        l.thickness = hi - lo;
        for (size_t p = 0; p < parts.size(); ++p)
            for (size_t s = 0; s < sources[p].size(); ++s) {
                const auto& src = sources[p][s];
                if (src.lo >= hi || src.hi <= lo || src.lo >= src.hi)
                    continue;
                // Sum fragment intersections before applying the area threshold:
                // splitting a ring must not hide a geometric overlap.
                struct Overlap {
                    std::string body, material, name;
                    Real area = 0;
                };
                std::vector<Overlap> overlaps;
                for (const auto& previous : l.regions) {
                    auto found = std::find_if(overlaps.begin(), overlaps.end(), [&](const auto& x) {
                        return x.body == previous.body && x.material == previous.material;
                    });
                    if (found == overlaps.end()) {
                        overlaps.push_back({previous.body, previous.material, previous.name, 0});
                        found = std::prev(overlaps.end());
                    }
                    for (const auto& current : src.regions)
                        found->area += intersection(previous.footprint, current);
                }
                for (const auto& overlap : overlaps)
                    if (overlap.material != parts[p].material &&
                        overlap.area > options.tau_a * options.tau_a)
                        result.defects.push_back(diagnostic(Tier::C,
                                                            DefectReason::PART_OVERLAP,
                                                            parts[p].id + "#" + std::to_string(s) +
                                                                "/overlap/" + overlap.name,
                                                            "overlap with " + overlap.body));
                for (size_t f = 0; f < src.regions.size(); ++f) {
                    Inclusion region{parts[p].id + "#" + std::to_string(s) + "#" +
                                         std::to_string(f),
                                     parts[p].material,
                                     parts[p].id,
                                     src.regions[f]};
                    size_t count = 0, index = 0;
                    for (size_t j = 1; j < zs.size(); ++j)
                        if (src.lo < zs[j] && src.hi > zs[j - 1] && src.lo < src.hi) {
                            if (j < i)
                                ++index;
                            ++count;
                        }
                    if (count > 1)
                        region.name += "#" + std::to_string(index);
                    Real a = area(polygon(region.footprint));
                    Real inside = intersection(region.footprint, base.outline);
                    // Exact containment in a box avoids subtracting independently
                    // rounded areas, without hiding even a one-ULP outside sliver.
                    const auto& box = region.footprint.box;
                    const auto& outline = base.outline;
                    bool contained = outline.polygon.empty() && box[0] >= outline.box[0] &&
                                     box[1] >= outline.box[1] && box[2] <= outline.box[2] &&
                                     box[3] <= outline.box[3];
                    // Polygon clipping sums independently rounded fragment areas.
                    // Bound that roundoff relative to the queried area and outline
                    // complexity; box outlines retain the exact comparison above.
                    Real roundoff = outline.polygon.empty()
                                        ? 0
                                        : 32 * std::numeric_limits<Real>::epsilon() *
                                              outline.polygon.size() * std::max(a, inside);
                    if (!contained && a - inside > roundoff)
                        result.defects.push_back(diagnostic(Tier::C,
                                                            DefectReason::PART_OUTSIDE_OUTLINE,
                                                            region.name + "/outside",
                                                            "region outside fixed outline"));
                    l.regions.push_back(std::move(region));
                }
            }
        result.stack.layers.push_back(std::move(l));
    }
    // Bodies intentionally repeat across split pieces, but generated names must
    // remain distinct from every output name and from the part body identifiers.
    std::set<std::string> output_names = ids;
    for (const auto& layer : result.stack.layers) {
        if (!output_names.insert(layer.name).second)
            return invalid("generated name collides with " + layer.name);
        for (const auto& region : layer.regions)
            if (!output_names.insert(region.name).second)
                return invalid("generated name collides with " + region.name);
    }
    return result;
}
} // namespace chipcae::geometry
