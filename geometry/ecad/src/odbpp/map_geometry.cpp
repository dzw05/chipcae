#include "map.hpp"

#include <algorithm>
#include <clipper2/clipper.h>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace chipcae::geometry::ecad::odbpp_detail {
namespace {
using namespace detail;
using namespace Clipper2Lib;
constexpr double pi = std::numbers::pi;
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
        // Use radius in IPC's MM tolerance formula so rotation cannot change resolution.
        const double tol = std::max(1e-6, 0.01 * std::max(1.0, radius / 1000)) * 1000;
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
    Path64 contour(const OdbContour& c) const {
        Path64 p{point(c.start)};
        auto s = c.start;
        for (const auto& seg : c.segs) {
            if (seg.is_arc)
                arc(p, s, seg.end, seg.center, seg.cw);
            else
                p.push_back(point(seg.end));
            s = seg.end;
        }
        if (p.size() > 1 && p.front() == p.back())
            p.pop_back();
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
        for (const auto& kind : {std::string("round"),
                                 std::string("rect"),
                                 std::string("oval"),
                                 std::string("r"),
                                 std::string("s")})
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
            if (s.kind == "r")
                s.kind = "round";
            if (s.kind == "s")
                s.kind = "rect";
            return s;
        } catch (const std::exception&) {
            return {};
        }
    }
    Paths64 widen(const Path64& p, const Symbol& s) const {
        ClipperOffset offset;
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
        double a = p.rot_deg * pi / 180;
        for (auto& v : local) {
            double x = static_cast<double>(v.x) * (p.mirror ? -1 : 1), y = static_cast<double>(v.y);
            v = {std::llround(p.at.x * scale + x * std::cos(a) - y * std::sin(a)),
                 std::llround(p.at.y * scale + x * std::sin(a) + y * std::cos(a))};
        }
        if (Area(local) < 0)
            std::reverse(local.begin(), local.end());
        return {local};
    }
    void copper(EcadLayer& layer, const OdbFeatures& f) {
        Paths64 positive, negative;
        auto add = [&](Paths64 paths, bool polarity) {
            auto& dst = polarity ? positive : negative;
            dst.insert(dst.end(), paths.begin(), paths.end());
        };
        for (const auto& surface : f.surfaces) {
            Paths64 paths;
            for (const auto& c : surface.contours)
                paths.push_back(contour(c));
            add(Union(paths, FillRule::NonZero), surface.positive);
        }
        for (const auto& p : f.pads) {
            auto s = symbol(f, p.sym);
            if (s.kind.empty())
                unsupported("Symbol:" + layer.name + ":" + std::to_string(p.sym));
            else
                add(pad(p, s), p.positive);
        }
        for (const auto& l : f.lines) {
            auto s = symbol(f, l.sym);
            if (s.kind.empty())
                unsupported("Symbol:" + layer.name + ":" + std::to_string(l.sym));
            else
                add(widen({point(l.s), point(l.e)}, s), l.positive);
        }
        for (const auto& a : f.arcs) {
            auto s = symbol(f, a.sym);
            if (s.kind.empty())
                unsupported("Symbol:" + layer.name + ":" + std::to_string(a.sym));
            else {
                Path64 p{point(a.s)};
                arc(p, a.s, a.e, a.c, a.cw);
                add(widen(p, s), a.positive);
            }
        }
        for (auto path : Difference(Union(positive, FillRule::NonZero),
                                    Union(negative, FillRule::NonZero),
                                    FillRule::NonZero)) {
            StripDuplicates(path, true);
            if (path.size() < 3 || Area(path) == 0)
                continue;
            Polygon p;
            for (const auto& v : path)
                p.points.push_back({v.x, v.y});
            layer.polygons.push_back(std::move(p));
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
            auto s = symbol(f, p.sym);
            const OdbTool* tool = nullptr;
            auto tools = job.tools.find(layer.name);
            if (tools != job.tools.end())
                for (const auto& t : tools->second)
                    if (std::abs(t.finish_size * scale - s.w) < 1) {
                        tool = &t;
                        break;
                    }
            if (s.kind != "round" || (tool && fold_ascii(tool->type) == "slot")) {
                unsupported("Slot:" + layer.name);
                continue;
            }
            if (tool) {
                auto type = fold_ascii(tool->type);
                std::erase_if(type, [](char c) { return c == '-' || c == '_'; });
                if (type == "nonplated")
                    continue;
            }
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
            v.outer_radius_dbu = tool ? tool->finish_size * scale / 2 : s.w / 2;
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
        rasterize_vias(model, model.via_fractions);
    }
};
} // namespace
ImportResult map_odb(const OdbJob& job, const MapOptions& options) {
    ImportResult result;
    result.model.id = job.step;
    result.model.dbu_m = 1e-6;
    try {
        map_stackup(job, options, result);
        Mapper m{job, result.model, unit_m(job) / result.model.dbu_m};
        for (const auto& c : job.profile.contours) {
            if (c.is_hole)
                m.unsupported("ProfileHole");
            else if (result.model.outline.points.empty())
                for (const auto& p : m.contour(c))
                    result.model.outline.points.push_back({p.x, p.y});
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
        result.error = "IMPORT_REJECTED: " + std::string(e.what());
    }
    return result;
}
} // namespace chipcae::geometry::ecad::odbpp_detail
