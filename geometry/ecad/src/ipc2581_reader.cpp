#include "chipcae/geometry/ecad/ipc2581.hpp"

#include <algorithm>
#include <charconv>
#include <clipper2/clipper.h>
#include <cmath>
#include <expat.h>
#include <fstream>
#include <map>
#include <memory>
#include <string_view>

namespace chipcae::geometry::ecad {
namespace {
struct Parser {
    EcadModel model;
    std::vector<std::string> path;
    std::map<std::string, std::size_t> layer_index;
    std::map<std::string, std::pair<std::string, bool>> layer_defs;
    double units_to_m = 1e-3;
    std::string current_layer;
    struct Trace {
        Clipper2Lib::Path64 path;
        double width = 0;
    };
    Trace trace;
    std::map<std::string, double> line_widths;
    std::string active_line_desc;
    Polygon* active = nullptr;
    std::vector<double> x_values;
    std::string failure;
    XML_Parser parser = nullptr;

    static std::string local(const char* n) {
        std::string_view s(n);
        auto p = s.rfind(':');
        return std::string(p == s.npos ? s : s.substr(p + 1));
    }
    static const char* attr(const XML_Char** a, const char* key) {
        for (std::size_t i = 0; a && a[i]; i += 2)
            if (local(a[i]) == key)
                return a[i + 1];
        return nullptr;
    }
    static double number(const char* s, double fallback = 0) {
        if (!s)
            return fallback;
        char* end = nullptr;
        const double v = std::strtod(s, &end);
        return end && *end == '\0' && std::isfinite(v) ? v : fallback;
    }
    void start(const char* raw, const XML_Char** a) {
        const auto name = local(raw);
        path.push_back(name);
        const auto parent = path.size() > 1 ? path[path.size() - 2] : std::string{};
        if (name == "StackupLayer") {
            EcadLayer l;
            const char* ref = attr(a, "layerOrGroupRef");
            l.name = ref ? ref : "layer" + std::to_string(model.layers.size());
            l.material = attr(a, "matDes") ? attr(a, "matDes") : "";
            if (auto def = layer_defs.find(l.name); def != layer_defs.end()) {
                l.function = def->second.first;
                l.copper = def->second.second;
            }
            l.thickness_m = number(attr(a, "thickness"));
            l.thickness_m *= units_to_m;
            l.copper = l.copper || l.function.find("SIGNAL") != std::string::npos ||
                       l.function.find("PLANE") != std::string::npos;
            const auto index = model.layers.size();
            layer_index[l.name] = index;
            model.layers.push_back(std::move(l));
            current_layer.clear();
        } else if (name == "Layer") {
            const char* n = attr(a, "name");
            const char* f = attr(a, "layerFunction");
            if (n && f)
                layer_defs[n] = {f,
                                 (std::string_view(f).find("SIGNAL") != std::string_view::npos ||
                                  std::string_view(f).find("PLANE") != std::string_view::npos)};
        } else if (name == "CadHeader") {
            const char* unit = attr(a, "units");
            if (unit && std::string_view(unit) == "MICRON")
                units_to_m = 1e-6;
            else if (unit && std::string_view(unit) == "INCH")
                units_to_m = 0.0254;
        } else if (name == "Profile") {
            active = &model.outline;
        } else if (name == "LayerFeature") {
            const char* n = attr(a, "layerRef");
            current_layer = n ? n : "";
            auto it = layer_index.find(current_layer);
            if (it != layer_index.end())
                model.layers[it->second].copper = true;
        } else if (name == "Polygon") {
            active = nullptr;
            auto it = layer_index.find(current_layer);
            if (it != layer_index.end()) {
                model.layers[it->second].copper = true;
                model.layers[it->second].polygons.emplace_back();
                active = &model.layers[it->second].polygons.back();
            } else if (!model.outline.points.empty() || parent == "Profile")
                active = &model.outline;
        } else if (name == "LineDesc") {
            const char* id = attr(a, "id");
            if (id) {
                line_widths[id] = number(attr(a, "lineWidth"));
                trace.width = number(attr(a, "lineWidth"));
            }
        } else if (name == "LineDescRef") {
            const char* id = attr(a, "id");
            active_line_desc = id ? id : "";
        } else if (name == "Line" || name == "Arc") {
            trace = Trace{};
            const double coord_scale = units_to_m / 1e-6;
            trace.path.push_back(
                {static_cast<std::int64_t>(std::llround(number(attr(a, "startX")) * coord_scale)),
                 static_cast<std::int64_t>(std::llround(number(attr(a, "startY")) * coord_scale))});
            if (name == "Line")
                trace.path.push_back(
                    {static_cast<std::int64_t>(std::llround(number(attr(a, "endX")) * coord_scale)),
                     static_cast<std::int64_t>(
                         std::llround(number(attr(a, "endY")) * coord_scale))});
            else {
                const double sx = number(attr(a, "startX")), sy = number(attr(a, "startY")),
                             ex = number(attr(a, "endX")), ey = number(attr(a, "endY"));
                const double cx = number(attr(a, "centerX")), cy = number(attr(a, "centerY")),
                             radius = std::hypot(sx - cx, sy - cy);
                double begin = std::atan2(sy - cy, sx - cx), finish = std::atan2(ey - cy, ex - cx),
                       sweep = finish - begin;
                const char* cw = attr(a, "clockwise");
                if (cw && std::string_view(cw) == "true") {
                    while (sweep >= 0)
                        sweep -= 2 * std::acos(-1.0);
                } else {
                    while (sweep <= 0)
                        sweep += 2 * std::acos(-1.0);
                }
                const double tol = std::max(1e-6, 0.01 * std::max(1.0, std::abs(ey - sy)));
                const double step = radius > tol
                                        ? 2 * std::acos(std::clamp(1 - tol / radius, -1.0, 1.0))
                                        : std::abs(sweep);
                const int count = std::max(
                    1, static_cast<int>(std::ceil(std::abs(sweep) / std::max(step, 1e-6))));
                for (int i = 1; i <= count; ++i) {
                    const double t = begin + sweep * i / count;
                    trace.path.push_back({static_cast<std::int64_t>(std::llround(
                                              (cx + radius * std::cos(t)) * coord_scale)),
                                          static_cast<std::int64_t>(std::llround(
                                              (cy + radius * std::sin(t)) * coord_scale))});
                }
            }
        } else if (name == "PolyBegin" || name == "PolyStepSegment" || name == "Location" ||
                   name == "Point") {
            const char *x = attr(a, "x"), *y = attr(a, "y");
            if (x && y) {
                auto px = number(x), py = number(y);
                const double scale = units_to_m / 1e-6;
                if (active)
                    active->points.push_back({static_cast<std::int64_t>(std::llround(px * scale)),
                                              static_cast<std::int64_t>(std::llround(py * scale))});
            }
        } else if (name == "Step") {
            if (const char* id = attr(a, "name"))
                model.id = id;
        } else if (name != "IPC-2581" && name != "Ecad" && name != "CadData" && name != "Stackup" &&
                   name != "Set" && name != "Content" && name != "Step" && name != "Profile" &&
                   name != "LayerFeature" && name != "Polygon" && name != "PolyBegin" &&
                   name != "PolyStepSegment" && name != "Location" && name != "Point" &&
                   name != "Layer" && name != "StackupGroup" && name != "Spec" &&
                   name != "HistoryRecord" && name != "LogisticHeader" && name != "Bom" &&
                   name != "Line" && name != "Arc" && name != "LineDesc" && name != "LineDescRef" &&
                   name != "CadHeader") {
            model.report.unsupported.push_back(
                {"IMPORT_UNSUPPORTED_ELEMENT",
                 name,
                 static_cast<std::uint64_t>(XML_GetCurrentLineNumber(parser))});
        }
    }
    void end(const char* raw) {
        auto n = local(raw);
        if (n == "LineDescRef") {
            auto it = line_widths.find(active_line_desc);
            if (it != line_widths.end())
                trace.width = it->second;
            active_line_desc.clear();
        }
        if ((n == "Line" || n == "Arc") && trace.path.size() >= 2 && trace.width > 0) {
            auto li = layer_index.find(current_layer);
            if (li != layer_index.end()) {
                Clipper2Lib::ClipperOffset offset;
                offset.AddPath(
                    trace.path, Clipper2Lib::JoinType::Round, Clipper2Lib::EndType::Round);
                Clipper2Lib::Paths64 inflated;
                offset.Execute(trace.width * units_to_m / 2e-6, inflated);
                auto& dst = model.layers[li->second].polygons;
                for (const auto& path : inflated) {
                    Polygon polygon;
                    for (const auto& p : path)
                        polygon.points.push_back({p.x, p.y});
                    if (polygon.points.size() >= 3)
                        dst.push_back(std::move(polygon));
                }
                model.layers[li->second].copper = true;
            }
        }
        if (n == "Polygon" || n == "Profile")
            active = nullptr;
        if (n == "LayerFeature" || n == "StackupLayer")
            current_layer.clear();
        if (!path.empty())
            path.pop_back();
    }
    static void XMLCALL on_start(void* u, const XML_Char* n, const XML_Char** a) {
        static_cast<Parser*>(u)->start(n, a);
    }
    static void XMLCALL on_end(void* u, const XML_Char* n) { static_cast<Parser*>(u)->end(n); }
};
} // namespace
ImportResult read_ipc2581(const std::filesystem::path& file) {
    ImportResult result;
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        result.error = "cannot open IPC-2581 file";
        return result;
    }
    Parser state;
    state.parser = XML_ParserCreateNS(nullptr, ':');
    if (!state.parser) {
        result.error = "cannot create Expat parser";
        return result;
    }
    XML_SetUserData(state.parser, &state);
    XML_SetElementHandler(state.parser, Parser::on_start, Parser::on_end);
    char buffer[64 * 1024];
    while (in) {
        in.read(buffer, sizeof(buffer));
        const auto got = in.gcount();
        if (XML_Parse(state.parser, buffer, static_cast<int>(got), in.eof()) == XML_STATUS_ERROR) {
            result.error = XML_ErrorString(XML_GetErrorCode(state.parser));
            XML_ParserFree(state.parser);
            return result;
        }
    }
    XML_ParserFree(state.parser);
    result.model = std::move(state.model);
    for (auto& l : result.model.layers)
        if (l.function.find("SIGNAL") != std::string::npos ||
            l.function.find("PLANE") != std::string::npos)
            l.copper = true;
    return result;
}
} // namespace chipcae::geometry::ecad
