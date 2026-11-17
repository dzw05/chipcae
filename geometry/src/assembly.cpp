#include "chipcae/geometry/assembly.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>

// SP0 contract stub; SP3 supplies the implementation.
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
bool valid_slabs(const std::vector<Slab>& slabs) {
    if (slabs.empty())
        return false;
    for (const auto& s : slabs) {
        if (!std::isfinite(s.z0) || !std::isfinite(s.z1) || s.z0 >= s.z1 || s.sections.empty())
            return false;
        for (const auto& section : s.sections)
            if (!valid_ring(section.outer) ||
                !std::all_of(section.holes.begin(), section.holes.end(), valid_ring))
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
bool rectangle(const Polygon& p) {
    if (p.size() != 4)
        return false;
    for (size_t i = 0; i < 4; ++i) {
        Point a = p[i], b = p[(i + 1) % 4];
        if (a == b || (a[0] != b[0] && a[1] != b[1]))
            return false;
    }
    return true;
}

// Horizontal cuts include every hole's leftmost/lowest vertex. On that line
// the left bridge ends at the nearest outer edge; extending the cuts through
// the section avoids doubled bridge edges (weakly simple polygons). Between
// consecutive vertex heights the even/odd fill consists of convex trapezoids.
// Their interiors are disjoint, so areas and intersections can be summed.
std::vector<Polygon> fragments(const SlabSection& section) {
    std::vector<const Ring*> rings{&section.outer};
    std::vector<const Ring*> holes;
    auto anchor = [](const Ring* r) { return *std::min_element(r->pts.begin(), r->pts.end()); };
    for (const auto& h : section.holes)
        holes.push_back(&h);
    std::stable_sort(
        holes.begin(), holes.end(), [&](auto a, auto b) { return anchor(a) < anchor(b); });
    rings.insert(rings.end(), holes.begin(), holes.end());
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
std::vector<Footprint> footprints(const SlabSection& s) {
    if (s.holes.empty())
        return {footprint(s.outer.pts, rectangle(s.outer.pts))};
    std::vector<Footprint> out;
    for (const auto& p : fragments(s))
        out.push_back(footprint(p, false));
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
    if (a.box[0] >= b.box[2] || b.box[0] >= a.box[2] || a.box[1] >= b.box[3] ||
        b.box[1] >= a.box[3])
        return 0;
    Real sum = 0;
    auto aa = fragments({Ring{polygon(a)}, {}});
    auto bb = fragments({Ring{polygon(b)}, {}});
    for (const auto& x : aa)
        for (const auto& y : bb)
            sum += convex_intersection(x, y);
    return sum;
}
Defect diagnostic(
    Tier tier, DefectReason reason, const std::string& id, Real size, const std::string& details) {
    Defect d{};
    d.tier = tier;
    d.reason = reason;
    d.defect_id = id;
    d.size = size;
    d.dimension = reason == DefectReason::THIN_LAYER_MERGED ? "thickness" : "area";
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
        if (!std::isfinite(attach.gap) || attach.gap < 0 ||
            (attach.bond && (!std::isfinite(attach.bond->thickness) ||
                             attach.bond->thickness <= 0 || attach.bond->thickness != attach.gap)))
            return invalid("bond requires a positive thickness equal to gap");
        Real face = 0;
        if (attach.target.rfind("layer:", 0) == 0) {
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
        } else if (attach.target.rfind("part:", 0) == 0 && attach.target.size() > 5 &&
                   attach.resolved_z) {
            face = *attach.resolved_z;
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
    if (!valid_slabs(local))
        return invalid("placement overflows or collapses geometry");
    return PlacedPart{"", "", std::move(local)};
}

Expected<MergeResult>
merge_parts(StackModel base, const std::vector<PlacedPart>& parts, const MergeOptions& options) {
    if (base.layers.empty())
        return invalid("base has no layers");
    MergeResult result{base, {}, 0};
    if (parts.empty())
        return result;
    if (!std::isfinite(options.t_min) || !std::isfinite(options.eps_weld) ||
        !std::isfinite(options.tau_a) || options.eps_weld <= 0 ||
        options.t_min < options.eps_weld || options.tau_a < 0)
        return invalid("invalid merge tolerances");
    std::set<std::string> ids, layer_names;
    std::vector<Real> boundaries{0};
    for (const auto& l : base.layers) {
        if (!std::isfinite(l.thickness) || l.thickness <= 0 || !layer_names.insert(l.name).second)
            return invalid("invalid base layer thickness or duplicate name");
        boundaries.push_back(boundaries.back() + l.thickness);
    }
    for (const auto& p : parts)
        if (p.id.empty() || !ids.insert(p.id).second || !valid_slabs(p.slabs))
            return invalid("empty slabs, invalid geometry or duplicate part id");
    std::set<Real> interfaces(boundaries.begin(), boundaries.end());
    // Establish all ordinary candidate interfaces in sorted order, independent
    // of part precedence. Base interfaces always have priority over new ones.
    std::set<Real> candidates;
    for (const auto& p : parts)
        for (const auto& s : p.slabs) {
            candidates.insert(s.z0);
            candidates.insert(s.z1);
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
    struct Source {
        Real lo, hi;
        std::vector<Footprint> regions;
    };
    std::vector<std::vector<Source>> sources;
    for (Real z : candidates)
        if (aligned(z) != z)
            ++result.welded_interfaces;
    std::set<Real> original_interfaces = candidates;
    original_interfaces.insert(boundaries.begin(), boundaries.end());
    for (const auto& p : parts) {
        std::vector<Source> slabs;
        size_t thin_index = 0;
        bool bond = p.id.size() >= 5 && p.id.compare(p.id.size() - 5, 5, ".bond") == 0;
        for (const auto& s : p.slabs) {
            Real lo = aligned(s.z0), hi = aligned(s.z1);
            // Preserve per-source provenance for each retained interval before
            // alignment, including thin pieces created by a base boundary.
            std::vector<Real> cuts{s.z0};
            for (Real z : original_interfaces)
                if (z > s.z0 && z < s.z1)
                    cuts.push_back(z);
            cuts.push_back(s.z1);
            const size_t previous_thin_index = thin_index;
            auto report_thin = [&](Real before, Real after) {
                std::ostringstream details;
                details << std::setprecision(17) << "original thickness " << before
                        << "; resulting thickness " << after;
                result.defects.push_back(diagnostic(Tier::B,
                                                    DefectReason::THIN_LAYER_MERGED,
                                                    p.id + "/thin#" + std::to_string(thin_index++),
                                                    before,
                                                    details.str()));
            };
            for (size_t i = 1; i < cuts.size(); ++i) {
                Real t = cuts[i] - cuts[i - 1];
                if (t < options.t_min && (bond || t >= options.eps_weld)) {
                    Real after = std::max(Real(0), aligned(cuts[i]) - aligned(cuts[i - 1]));
                    report_thin(t, after);
                }
            }
            // A boundary just outside a source can also change its thickness;
            // no internal cut represents that thin displacement. Numerical
            // welding alone remains silent, but larger changes need provenance.
            if (previous_thin_index == thin_index && (std::abs(lo - s.z0) >= options.eps_weld ||
                                                      std::abs(hi - s.z1) >= options.eps_weld))
                report_thin(s.z1 - s.z0, std::max(Real(0), hi - lo));
            Source source{lo, hi, {}};
            for (const auto& section : s.sections) {
                auto fs = footprints(section);
                source.regions.insert(source.regions.end(), fs.begin(), fs.end());
            }
            slabs.push_back(std::move(source));
        }
        sources.push_back(std::move(slabs));
    }
    // Eliminated sources must not leave a remote interface that extends the
    // stack with a phantom void layer. Keep the base extent and the extents of
    // surviving sources, including genuine void gaps between them.
    Real bottom = 0, top = boundaries.back();
    for (const auto& part_sources : sources)
        for (const auto& source : part_sources)
            if (source.lo < source.hi) {
                bottom = std::min(bottom, source.lo);
                top = std::max(top, source.hi);
            }
    std::vector<Real> zs;
    for (Real z : interfaces)
        if (z >= bottom && z <= top)
            zs.push_back(z);
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
    auto unique_name = [&](std::string name) {
        std::string original = name;
        size_t suffix = 0;
        while (!used_names.insert(name).second)
            name = original + "#" + std::to_string(suffix++);
        return name;
    };
    for (size_t i = 1; i < zs.size(); ++i) {
        Real lo = zs[i - 1], hi = zs[i];
        size_t o = owner(lo + (hi - lo) / 2);
        Layer l;
        if (o < base.layers.size()) {
            l = base.layers[o];
            if (split_counts[o] > 1)
                l.name = unique_name(l.name + "#" + std::to_string(split_indices[o]++));
        } else {
            l.name = unique_name("assembly#" + std::to_string(i - 1));
            l.kind = "Explicit";
        }
        l.thickness = hi - lo;
        for (size_t p = 0; p < parts.size(); ++p)
            for (size_t s = 0; s < sources[p].size(); ++s) {
                const auto& src = sources[p][s];
                if (src.lo >= hi || src.hi <= lo || src.lo >= src.hi)
                    continue;
                // Sum fragment intersections before applying the area threshold:
                // splitting a ring must not hide a material overlap.
                struct Overlap {
                    std::string body, material, name;
                    Real area = 0;
                };
                std::vector<Overlap> overlaps;
                for (const auto& previous : l.regions) {
                    if (previous.material == parts[p].material)
                        continue;
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
                    if (overlap.area > options.tau_a * options.tau_a)
                        result.defects.push_back(diagnostic(Tier::C,
                                                            DefectReason::PART_OVERLAP,
                                                            parts[p].id + "#" + std::to_string(s) +
                                                                "/overlap/" + overlap.name,
                                                            overlap.area,
                                                            "overlap with " + overlap.body));
                for (size_t f = 0; f < src.regions.size(); ++f) {
                    Inclusion region{parts[p].id + "#" + std::to_string(s) + "#" +
                                         std::to_string(f),
                                     parts[p].material,
                                     parts[p].id,
                                     src.regions[f]};
                    Real a = area(polygon(region.footprint));
                    Real inside = intersection(region.footprint, base.outline);
                    if (a - inside > std::numeric_limits<Real>::epsilon() * a * 64)
                        result.defects.push_back(diagnostic(Tier::C,
                                                            DefectReason::PART_OUTSIDE_OUTLINE,
                                                            region.name + "/outside",
                                                            a - inside,
                                                            "region outside fixed outline"));
                    l.regions.push_back(std::move(region));
                }
            }
        result.stack.layers.push_back(std::move(l));
    }
    return result;
}
} // namespace chipcae::geometry
