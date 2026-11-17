#include "chipcae/geometry/ecad/ipc2581.hpp"

#include <algorithm>
#include <charconv>
#include <clipper2/clipper.h>
#include <cmath>
#include <expat.h>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string_view>

namespace chipcae::geometry::ecad {
namespace {
struct Parser {
    EcadModel model;
    struct HoleDef {
        double diameter = 0;
        bool plated = false;
    };
    std::map<std::string, HoleDef> holes;
    std::map<std::string, Component> packages;
    using SourcePoint = std::array<double, 2>;
    std::map<std::string, std::vector<SourcePoint>> package_points, package_pins;
    std::vector<SourcePoint> component_origins;
    SourcePoint source_point(const XML_Char** a) const {
        return {number(attr(a, "x")) * scale(), number(attr(a, "y")) * scale()};
    }
    std::map<std::string, std::pair<std::string, std::string>> spans;
    std::map<std::string, std::string> layer_specs;
    std::map<std::string, double> plating;
    std::string definition_layer, package, padstack, spec;
    struct Pending {
        Via via;
        std::string layer, spec;
        double diameter = 0;
        bool plated = true;
    };
    std::vector<Pending> pending;
    bool in_hole = false, in_pad = false, in_component = false;
    double scale() const { return units_to_m / model.dbu_m; }
    static std::string value(const XML_Char** a, const char* key) {
        auto v = attr(a, key);
        return v ? v : "";
    }
    std::array<std::int64_t, 2> point(const XML_Char** a) const {
        return {std::llround(number(attr(a, "x")) * scale()),
                std::llround(number(attr(a, "y")) * scale())};
    }
    std::map<std::size_t, std::string> stack_specs;
    std::set<std::size_t> explicit_materials;
    std::map<std::string, std::string> materials;
    std::size_t material_general_depth = 0;
    enum class Kind { copper, dielectric, skipped, mask, unknown };
    static std::string uppercase(std::string s) {
        for (char& c : s)
            if (c >= 'a' && c <= 'z')
                c -= 'a' - 'A';
        return s;
    }
    static Kind classify(const std::string& raw) {
        const auto f = uppercase(raw);
        if (f == "SOLDERMASK")
            return Kind::mask;
        for (const auto* name : {"SILKSCREEN",
                                 "LEGEND",
                                 "SOLDERPASTE",
                                 "PASTEMASK",
                                 "DOCUMENT",
                                 "ASSEMBLY",
                                 "COURTYARD",
                                 "GLUE",
                                 "BOARD_OUTLINE"})
            if (f == name)
                return Kind::skipped;
        if (f.rfind("DRILL", 0) == 0)
            return Kind::skipped;
        for (const auto* name : {"CONDUCTOR", "SIGNAL", "PLANE", "MIXED"})
            if (f.find(name) != std::string::npos)
                return Kind::copper;
        return f.rfind("DIEL", 0) == 0 ? Kind::dielectric : Kind::unknown;
    }
    bool skipped(const std::string& name) const {
        const auto it = layer_defs.find(name);
        if (it == layer_defs.end())
            return false;
        const auto k = classify(it->second.first);
        return k != Kind::copper && k != Kind::dielectric;
    }
    // Resolve deferred definitions and compact layers before computing any via indices.
    void resolve() {
        std::vector<EcadLayer> retained;
        std::vector<UnsupportedElement> additions;
        std::set<std::string> seen;
        layer_index.clear();
        for (std::size_t i = 0; i < model.layers.size(); ++i) {
            auto& layer = model.layers[i];
            auto d = layer_defs.find(layer.name);
            if (d != layer_defs.end()) {
                if (layer.function.empty())
                    layer.function = d->second.first;
                const auto kind = classify(layer.function);
                const bool first = seen.insert(layer.name).second;
                if (kind != Kind::copper && kind != Kind::dielectric) {
                    if (first) {
                        const auto& f = layer.function;
                        const auto k = classify(f);
                        const auto element = k == Kind::mask ? "SolderMask:" + layer.name
                                             : k == Kind::skipped
                                                 ? "SkippedLayer:" + layer.name + ":" + f
                                                 : "LayerFunction:" + f + ":" + layer.name;
                        additions.push_back({"IMPORT_UNSUPPORTED_ELEMENT", element, 0});
                    }
                    continue;
                }
                layer.copper = layer.copper || classify(layer.function) == Kind::copper;
            }
            if (layer.material.empty() && !explicit_materials.count(i)) {
                const auto ref = stack_specs.find(i);
                if (ref != stack_specs.end()) {
                    const auto material = materials.find(ref->second);
                    if (material != materials.end())
                        layer.material = material->second;
                }
            }
            layer_index[layer.name] = retained.size();
            retained.push_back(std::move(layer));
        }
        model.layers = std::move(retained);
        model.report.unsupported.insert(
            model.report.unsupported.begin(), additions.begin(), additions.end());
        for (std::size_t ci = 0; ci < model.components.size(); ++ci) {
            auto& c = model.components[ci];
            auto pkg = packages.find(c.package);
            if (pkg == packages.end()) {
                model.report.unsupported.push_back(
                    {"IMPORT_UNSUPPORTED_ELEMENT", "PackageRef:" + c.package, 0});
                continue;
            }
            auto transform = [&](SourcePoint p) {
                const double t = c.rotation_degrees * std::acos(-1.0) / 180;
                const double x = c.mirror ? -static_cast<double>(p[0]) : static_cast<double>(p[0]);
                return std::array<std::int64_t, 2>{
                    std::llround(component_origins[ci][0] + x * std::cos(t) - p[1] * std::sin(t)),
                    std::llround(component_origins[ci][1] + x * std::sin(t) + p[1] * std::cos(t))};
            };
            c.outline = pkg->second.outline;
            c.pads = pkg->second.pads;
            for (std::size_t i = 0; i < c.outline.points.size(); ++i)
                c.outline.points[i] = transform(package_points[c.package][i]);
            for (std::size_t i = 0; i < c.pads.size(); ++i)
                c.pads[i].position = transform(package_pins[c.package][i]);
        }
        for (auto& p : pending) {
            if (!p.via.padstack.empty()) {
                auto h = holes.find(p.via.padstack);
                if (h == holes.end()) {
                    model.report.unsupported.push_back(
                        {"IMPORT_UNSUPPORTED_ELEMENT", "PadstackRef:" + p.via.padstack, 0});
                    continue;
                }
                p.diameter = h->second.diameter;
                p.plated = h->second.plated;
            }
            if (!p.plated)
                continue;
            auto span = spans.find(p.layer);
            if (span != spans.end()) {
                p.via.start_layer = span->second.first;
                p.via.end_layer = span->second.second;
            }
            auto a = layer_index.find(p.via.start_layer), b = layer_index.find(p.via.end_layer);
            auto thickness = plating.find(p.spec.empty() ? layer_specs[p.layer] : p.spec);
            if (a == layer_index.end() || b == layer_index.end() ||
                !model.layers[a->second].copper || !model.layers[b->second].copper ||
                thickness == plating.end() || p.diameter <= 0 || thickness->second <= 0) {
                model.report.unsupported.push_back(
                    {"IMPORT_UNSUPPORTED_ELEMENT", "ViaGeometry:" + p.via.id, 0});
                continue;
            }
            p.via.outer_radius_dbu = p.diameter / 2;
            p.via.inner_radius_dbu = std::max(0.0, p.diameter / 2 - thickness->second);
            for (auto i = std::min(a->second, b->second) + 1; i < std::max(a->second, b->second);
                 ++i)
                if (!model.layers[i].copper)
                    p.via.dielectric_layers.push_back(i);
            model.vias.push_back(std::move(p.via));
        }
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
    std::uint64_t line_number() const {
#if XML_MAJOR_VERSION > 2 || (XML_MAJOR_VERSION == 2 && XML_MINOR_VERSION >= 10)
        return XML_GetCurrentLineNumber64(parser);
#else
        return static_cast<std::uint64_t>(XML_GetCurrentLineNumber(parser));
#endif
    }
    void start(const char* raw, const XML_Char** a) {
        const auto name = local(raw);
        path.push_back(name);
        const auto parent = path.size() > 1 ? path[path.size() - 2] : std::string{};
        if (name == "Property" && !spec.empty() && material_general_depth &&
            materials[spec].empty() && !value(a, "text").empty())
            materials[spec] = value(a, "text");
        if (name == "Package") {
            package = value(a, "name");
            packages[package] = Component{};
            package_points[package].clear();
            package_pins[package].clear();
        } else if (name == "Component") {
            model.components.emplace_back();
            component_origins.push_back({});
            auto& c = model.components.back();
            c.id = value(a, "refDes");
            c.package = value(a, "packageRef");
            in_component = true;
        } else if (name == "Pin" && !package.empty()) {
            packages[package].pads.push_back({value(a, "number"), {}});
            package_pins[package].push_back({});
        } else if (name == "Xform" && in_component) {
            auto& c = model.components.back();
            c.rotation_degrees = number(attr(a, "rotation"));
            c.mirror = value(a, "mirror") == "true";
        } else if (name == "Location" && in_component) {
            model.components.back().position = point(a);
            component_origins.back() = source_point(a);
        } else if (name == "Location" && parent == "Pin" && !package.empty()) {
            packages[package].pads.back().position = point(a);
            package_pins[package].back() = source_point(a);
        } else if (name == "PadStackDef") {
            padstack = value(a, "name");
        } else if (name == "PadstackHoleDef") {
            holes[padstack] = {number(attr(a, "diameter")) * scale(),
                               value(a, "platingStatus") != "NONPLATED"};
        } else if (name == "Hole" || (name == "Pad" && attr(a, "padstackDefRef"))) {
            Pending p;
            p.layer = current_layer;
            p.via.id = value(a, "name");
            if (p.via.id.empty())
                p.via.id = "via" + std::to_string(pending.size());
            p.via.padstack = value(a, "padstackDefRef");
            p.via.position = point(a);
            p.diameter = number(attr(a, "diameter")) * scale();
            p.plated = value(a, "platingStatus") != "NONPLATED";
            pending.push_back(p);
            in_hole = name == "Hole";
            in_pad = name == "Pad";
        } else if (name == "Location" && in_pad) {
            pending.back().via.position = point(a);
        } else if (name == "Span") {
            spans[definition_layer] = {value(a, "fromLayer"), value(a, "toLayer")};
        } else if (name == "Spec") {
            spec = value(a, "name");
        } else if (name == "General" && !spec.empty() && value(a, "type") == "MATERIAL") {
            material_general_depth = path.size();
        } else if (name == "Property" && !spec.empty() && value(a, "name") == "PLATING_THICKNESS") {
            double unit = units_to_m;
            auto u = value(a, "unit");
            if (u == "MICRON")
                unit = 1e-6;
            else if (u == "INCH")
                unit = 0.0254;
            else if (u == "MILLIMETER")
                unit = 1e-3;
            plating[spec] = number(attr(a, "value")) * unit / model.dbu_m;
        } else if (name == "SpecRef") {
            if (in_hole)
                pending.back().spec = value(a, "id");
            else if (parent == "StackupLayer" && !model.layers.empty())
                stack_specs[model.layers.size() - 1] = value(a, "id");
            else if (parent == "Layer")
                layer_specs[definition_layer] = value(a, "id");
        } else if (name == "StackupLayer") {
            EcadLayer l;
            const char* ref = attr(a, "layerOrGroupRef");
            l.name = ref ? ref : "layer" + std::to_string(model.layers.size());
            l.material = attr(a, "matDes") ? attr(a, "matDes") : "";
            if (attr(a, "matDes"))
                explicit_materials.insert(model.layers.size());
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
            definition_layer = value(a, "name");
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
            active = package.empty() ? &model.outline : &packages[package].outline;
        } else if (name == "LayerFeature") {
            const char* n = attr(a, "layerRef");
            current_layer = n ? n : "";
            auto it = layer_index.find(current_layer);
            const auto def = layer_defs.find(current_layer);
            if (it != layer_index.end() && def != layer_defs.end() &&
                classify(def->second.first) == Kind::copper)
                model.layers[it->second].copper = true;
        } else if (name == "Polygon") {
            active = nullptr;
            auto it = layer_index.find(current_layer);
            if (!package.empty()) {
                active = &packages[package].outline;
            } else if (it != layer_index.end() && !skipped(current_layer)) {
                model.layers[it->second].copper = true;
                model.layers[it->second].polygons.emplace_back();
                active = &model.layers[it->second].polygons.back();
            } else if (current_layer.empty() &&
                       (!model.outline.points.empty() || parent == "Profile"))
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
                if (active && !package.empty())
                    package_points[package].push_back(source_point(a));
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
                   name != "CadHeader" && name != "Outline" && name != "General") {
            model.report.unsupported.push_back({"IMPORT_UNSUPPORTED_ELEMENT", name, line_number()});
        }
    }
    void end(const char* raw) {
        auto n = local(raw);
        if (n == "General" && path.size() == material_general_depth)
            material_general_depth = 0;
        if (n == "Package")
            package.clear();
        if (n == "Component")
            in_component = false;
        if (n == "Hole")
            in_hole = false;
        if (n == "Pad")
            in_pad = false;
        if (n == "PadStackDef")
            padstack.clear();
        if (n == "Layer")
            definition_layer.clear();
        if (n == "Spec")
            spec.clear();
        if (n == "LineDescRef") {
            auto it = line_widths.find(active_line_desc);
            if (it != line_widths.end())
                trace.width = it->second;
            active_line_desc.clear();
        }
        if ((n == "Line" || n == "Arc") && trace.path.size() >= 2 && trace.width > 0) {
            auto li = layer_index.find(current_layer);
            if (li != layer_index.end() && !skipped(current_layer)) {
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
    state.resolve();
    result.model = std::move(state.model);
    for (auto& l : result.model.layers)
        if (l.function.find("SIGNAL") != std::string::npos ||
            l.function.find("PLANE") != std::string::npos)
            l.copper = true;
    return result;
}
} // namespace chipcae::geometry::ecad
