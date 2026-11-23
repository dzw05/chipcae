#include "ipc2581_stream.hpp"

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
        bool plated = true, artwork = false;
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
        for (auto& t : deferred_traces)
            draw_trace(t);
        deferred_traces.clear();
        auto& reports = model.report.unsupported;
        reports.erase(std::remove_if(reports.begin(),
                                     reports.end(),
                                     [](const auto& r) { return r.element.empty(); }),
                      reports.end());
        std::vector<EcadLayer> retained;
        std::vector<UnsupportedElement> additions;
        std::set<std::string> seen, omitted_masks;
        auto resolve_material = [&](EcadLayer& layer, std::size_t i) {
            if (layer.material.empty() && !explicit_materials.count(i)) {
                const auto ref = stack_specs.find(i);
                if (ref != stack_specs.end()) {
                    const auto material = materials.find(ref->second);
                    if (material != materials.end())
                        layer.material = material->second;
                }
            }
        };
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
                        if (k == Kind::mask) {
                            omitted_masks.insert(layer.name);
                            resolve_material(layer, i);
                            model.omitted_masks.push_back({layer, retained.size()});
                        }
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
            resolve_material(layer, i);
            layer_index[layer.name] = retained.size();
            retained.push_back(std::move(layer));
        }
        model.layers = std::move(retained);
        for (const auto& artwork : deferred_artwork) {
            const auto& pad = pending[artwork.pad];
            auto li = layer_index.find(pad.layer);
            if (li == layer_index.end() || skipped(pad.layer))
                continue;
            const auto center = pad.via.position;
            Polygon polygon;
            for (int i = 0; i < 256; ++i) {
                const double angle = 2 * std::acos(-1.0) * i / 256;
                polygon.points.push_back(
                    {std::llround(center[0] + artwork.radius * std::cos(angle)),
                     std::llround(center[1] + artwork.radius * std::sin(angle))});
            }
            model.layers[li->second].polygons.push_back(std::move(polygon));
            circle_uses[artwork.use].modeled = true;
        }
        // Merge overlapping copper artwork and traces before computing their area.
        for (auto& layer : model.layers) {
            auto& polygons = layer.polygons;
            if (polygons.size() < 2)
                continue;
            Clipper2Lib::Paths64 paths;
            for (const auto& p : polygons) {
                if (p.points.size() < 3)
                    continue;
                Clipper2Lib::Path64 points;
                for (const auto& v : p.points)
                    points.push_back({v[0], v[1]});
                const double area = Clipper2Lib::Area(points);
                // Reject collapsed rings and sub-grid slivers before clipping (area in DBU^2).
                if (std::abs(area) < 1.0)
                    continue;
                if (area < 0)
                    std::reverse(points.begin(), points.end());
                paths.push_back(std::move(points));
            }
            polygons.clear();
            for (const auto& points : Clipper2Lib::Union(paths, Clipper2Lib::FillRule::NonZero)) {
                Polygon p;
                for (const auto& v : points)
                    p.points.push_back({v.x, v.y});
                polygons.push_back(std::move(p));
            }
        }
        model.report.unsupported.insert(
            model.report.unsupported.begin(), additions.begin(), additions.end());
        std::stable_sort(circle_uses.begin(), circle_uses.end(), [](const auto& a, const auto& b) {
            return a.line < b.line;
        });
        std::set<std::pair<std::string, std::uint64_t>> reported_circles;
        for (const auto& use : circle_uses) {
            // Solder mask is deliberately omitted and already has its own diagnostic.
            if (omitted_masks.count(use.layer))
                continue;
            if (!use.modeled && circles.count(use.id) &&
                reported_circles.emplace(use.id, use.line).second)
                reports.push_back({"IMPORT_UNSUPPORTED_ELEMENT", "Circle", use.line});
        }
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
        std::map<std::pair<std::size_t, std::size_t>, std::size_t> missing_plating;
        for (auto& p : pending) {
            // Copper pad instances describe artwork, not additional drilled vias.
            if (p.artwork)
                continue;
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
                p.diameter <= 0 || (thickness != plating.end() && thickness->second <= 0)) {
                model.report.unsupported.push_back(
                    {"IMPORT_UNSUPPORTED_ELEMENT", "ViaGeometry:" + p.via.id, 0});
                continue;
            }
            if (thickness == plating.end()) {
                ++missing_plating[{a->second, b->second}];
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
        for (const auto& [pair, count] : missing_plating)
            model.report.unsupported.push_back(
                {"IMPORT_UNSUPPORTED_ELEMENT",
                 "ViaPlatingMissing:" + model.layers[pair.first].name + ":" +
                     model.layers[pair.second].name + ":" + std::to_string(count),
                 0});
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
        bool inline_width = false, arc = false;
        std::string ref, layer, end;
        double unit = 1e-3;
        std::size_t slot = 0;
        std::uint64_t line = 0;
    };
    Trace trace;
    std::map<std::string, double> line_widths;
    bool in_trace = false;
    std::vector<Trace> deferred_traces;
    std::set<std::string> duplicate_ids, missing_ids, missing_layers, end_layers;
    std::map<std::string, double> circles;
    std::string standard;
    struct CircleUse {
        std::string id, layer;
        std::uint64_t line;
        bool modeled = false;
    };
    std::vector<CircleUse> circle_uses;
    struct Artwork {
        std::size_t pad, use;
        double radius;
    };
    std::vector<Artwork> deferred_artwork;
    std::size_t dictionary_user_depth = 0, entry_user_depth = 0;
    struct UserSpecial {
        std::size_t depth;
        std::uint64_t line;
        bool dictionary, layer, reported = false;
    };
    std::vector<UserSpecial> user_specials;
    void draw_trace(Trace& t, Clipper2Lib::EndType end = Clipper2Lib::EndType::Round) {
        auto li = layer_index.find(t.layer);
        if (li == layer_index.end() || skipped(t.layer))
            return;
        std::string problem;
        if (!t.inline_width && !t.ref.empty()) {
            auto w = line_widths.find(t.ref);
            if (w == line_widths.end()) {
                if (missing_ids.insert(t.ref).second)
                    problem = "LineDescMissing:" + t.ref;
            } else
                t.width = w->second;
        }
        if (t.width <= 0) {
            if ((t.inline_width || t.ref.empty() || line_widths.count(t.ref)) &&
                missing_layers.insert(t.layer).second)
                problem = "LineWidthMissing:" + t.layer;
            if (!problem.empty())
                model.report.unsupported[t.slot] = {"IMPORT_UNSUPPORTED_ELEMENT", problem, t.line};
            return;
        }
        const auto style = uppercase(t.end);
        if (!t.arc && style == "SQUARE")
            end = Clipper2Lib::EndType::Square;
        else if (!t.arc && style == "FLAT")
            end = Clipper2Lib::EndType::Butt;
        else if (!style.empty() && style != "ROUND" && end_layers.insert(t.layer).second)
            model.report.unsupported[t.slot] = {"IMPORT_UNSUPPORTED_ELEMENT",
                                                "LineEndUnsupported:" + t.layer + ":" + t.end,
                                                t.line};
        Clipper2Lib::ClipperOffset offset;
        // A collapsed ROUND line is a disk; keep its discretization error below 1e-3.
        if (t.path.size() == 2 && t.path.front() == t.path.back())
            offset.ArcTolerance(0.01);
        offset.AddPath(t.path, Clipper2Lib::JoinType::Round, end);
        Clipper2Lib::Paths64 inflated;
        offset.Execute(t.width * t.unit / 2e-6, inflated);
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
        if (name == "StandardPrimitiveRef" && in_pad)
            pending.back().artwork = true;
        const auto parent = path.size() > 1 ? path[path.size() - 2] : std::string{};
        if (name == "DictionaryUser")
            dictionary_user_depth = path.size();
        if (name == "EntryUser" && dictionary_user_depth)
            entry_user_depth = path.size();
        if (name == "UserSpecial") {
            user_specials.push_back(
                {path.size(), line_number(), entry_user_depth != 0, !current_layer.empty()});
            if (user_specials.back().dictionary || user_specials.back().layer)
                return;
        }
        if (entry_user_depth &&
            (name == "Polygon" || name == "PolyBegin" || name == "PolyStepSegment")) {
            // Dictionary shapes are definitions, never board, layer or package geometry.
            if (!user_specials.empty()) {
                auto& user = user_specials.back();
                if (user.dictionary && !user.reported) {
                    model.report.unsupported.push_back(
                        {"IMPORT_UNSUPPORTED_ELEMENT", "UserSpecial", user.line});
                    user.reported = true;
                }
            }
            return;
        }
        if (!user_specials.empty()) {
            const auto& user = user_specials.back();
            if (name == "Contour" && path.size() == user.depth + 1 &&
                (user.dictionary || user.layer))
                return;
            if (user.dictionary && (name == "Circle" || name == "LineDesc" || name == "FillDesc"))
                return;
        }
        if (name == "UserPrimitiveRef" && parent == "PadstackPadDef")
            return;
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
            else if (unit && std::string_view(unit) == "MILLIMETER")
                units_to_m = 1e-3;
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
        } else if (name == "EntryStandard") {
            model.report.unsupported.push_back({"IMPORT_UNSUPPORTED_ELEMENT", name, line_number()});
            standard = value(a, "id");
        } else if (name == "Circle" && !standard.empty()) {
            circles[standard] = number(attr(a, "diameter")) * scale();
        } else if (name == "StandardPrimitiveRef") {
            model.report.unsupported.push_back({"IMPORT_UNSUPPORTED_ELEMENT", name, line_number()});
            const bool definition =
                (!package.empty() && std::find(path.begin(), path.end(), "Pin") != path.end()) ||
                std::find(path.begin(), path.end(), "PadstackPadDef") != path.end();
            if (!current_layer.empty() && !definition) {
                const auto id = value(a, "id");
                circle_uses.push_back({id, current_layer, line_number()});
                // Preserve the existing requirement that the circle is already defined.
                if (in_pad && circles.count(id))
                    deferred_artwork.push_back(
                        {pending.size() - 1, circle_uses.size() - 1, circles[id] / 2});
            }
        } else if (name == "LineDesc") {
            const char* id = attr(a, "id");
            const double width = number(attr(a, "lineWidth"));
            if (id && !line_widths.emplace(id, width).second && duplicate_ids.insert(id).second)
                model.report.unsupported.push_back({"IMPORT_UNSUPPORTED_ELEMENT",
                                                    "LineDescDuplicate:" + std::string(id),
                                                    line_number()});
            if (in_trace) {
                trace.width = width;
                trace.inline_width = true;
                if (attr(a, "lineEnd"))
                    trace.end = value(a, "lineEnd");
            }
        } else if (name == "LineDescRef") {
            if (in_trace)
                trace.ref = value(a, "id");
        } else if (name == "Line" || name == "Arc") {
            trace = Trace{};
            in_trace = true;
            trace.arc = name == "Arc";
            trace.end = value(a, "lineEnd");
            trace.layer = current_layer;
            trace.unit = units_to_m;
            trace.line = line_number();
            trace.slot = model.report.unsupported.size();
            model.report.unsupported.push_back({});
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
        if (!user_specials.empty() && path.size() == user_specials.back().depth)
            user_specials.pop_back();
        if (path.size() == entry_user_depth)
            entry_user_depth = 0;
        if (path.size() == dictionary_user_depth)
            dictionary_user_depth = 0;
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
        if (n == "EntryStandard")
            standard.clear();
        if (n == "Line" || n == "Arc") {
            // Retain primitive order for first-problem reports, including forward references.
            deferred_traces.push_back(std::move(trace));
            in_trace = false;
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
struct detail::Ipc2581StreamParser::State {
    Parser parser;
    std::string error;
    State() {
        parser.parser = XML_ParserCreateNS(nullptr, ':');
        if (!parser.parser) {
            error = "cannot create Expat parser";
            return;
        }
        XML_SetUserData(parser.parser, &parser);
        XML_SetElementHandler(parser.parser, Parser::on_start, Parser::on_end);
    }
    ~State() {
        if (parser.parser)
            XML_ParserFree(parser.parser);
    }
};
detail::Ipc2581StreamParser::Ipc2581StreamParser() : state_(std::make_unique<State>()) {}
detail::Ipc2581StreamParser::~Ipc2581StreamParser() = default;
bool detail::Ipc2581StreamParser::feed(std::string_view chunk) {
    if (!state_->error.empty())
        return false;
    if (XML_Parse(state_->parser.parser, chunk.data(), static_cast<int>(chunk.size()), false) ==
        XML_STATUS_ERROR) {
        state_->error = XML_ErrorString(XML_GetErrorCode(state_->parser.parser));
        return false;
    }
    return true;
}
ImportResult detail::Ipc2581StreamParser::finish() {
    ImportResult result;
    if (state_->error.empty() && XML_Parse(state_->parser.parser, "", 0, true) == XML_STATUS_ERROR)
        state_->error = XML_ErrorString(XML_GetErrorCode(state_->parser.parser));
    if (!state_->error.empty()) {
        result.error = state_->error;
        return result;
    }
    auto& state = state_->parser;
    state.resolve();
    result.model = std::move(state.model);
    for (auto& l : result.model.layers)
        if (l.function.find("SIGNAL") != std::string::npos ||
            l.function.find("PLANE") != std::string::npos)
            l.copper = true;
    return result;
}
ImportResult read_ipc2581(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        ImportResult result;
        result.error = "cannot open IPC-2581 file";
        return result;
    }
    detail::Ipc2581StreamParser parser;
    char buffer[64 * 1024];
    while (in) {
        in.read(buffer, sizeof(buffer));
        if (!parser.feed(std::string_view(buffer, static_cast<std::size_t>(in.gcount()))))
            break;
    }
    return parser.finish();
}
} // namespace chipcae::geometry::ecad
