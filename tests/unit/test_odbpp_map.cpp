#include "map.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <numbers>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

static_assert(std::is_same_v<decltype(chipcae::geometry::ecad::odbpp_detail::map_odb(
                                 std::declval<const chipcae::geometry::ecad::detail::OdbJob&>())),
                             chipcae::geometry::Expected<chipcae::geometry::ecad::EcadModel>>);

using namespace chipcae::geometry::ecad;
using namespace chipcae::geometry::ecad::detail;
using namespace chipcae::geometry::ecad::odbpp_detail;
namespace {
void require(bool value, const std::string& message) {
    if (!value)
        throw std::runtime_error(message);
}
void near(double actual, double expected, double relative = 1e-6) {
    require(std::abs(actual - expected) <= std::abs(expected) * relative, "numeric mismatch");
}
OdbContour square(double size) {
    OdbContour contour;
    for (const OdbPoint end :
         {OdbPoint{size, 0}, OdbPoint{size, size}, OdbPoint{0, size}, OdbPoint{0, 0}}) {
        OdbSegment segment;
        segment.end = end;
        contour.segs.push_back(segment);
    }
    return contour;
}
// Synthetic KiCad-style stack table in mm; no vendor fixture or measured data.
OdbJob board() {
    OdbJob j;
    j.step = "board";
    const char* names[] = {"TOP", "CORE", "BOTTOM", "PREPREG"};
    const char* types[] = {"signal", "dielectric", "power_ground", "dielectric"};
    const char* thickness[] = {"0.035", "0.8", "0.035", "0.2"};
    for (int i = 0; i < 4; ++i) {
        OdbLayer l;
        l.name = names[i];
        l.type = types[i];
        l.row = i + 1;
        j.layers.push_back(l);
        j.attrs.layer[l.name][".layer_dielectric"] = thickness[i];
        if (i % 2)
            j.attrs.layer[l.name][".material"] = "sio2";
    }
    j.profile.contours.push_back(square(10));
    j.features["TOP"].surfaces.push_back({true, {square(8)}});
    return j;
}
EcadModel mapped(const OdbJob& j, const MapOptions& options = {}) {
    auto r = map_odb(j, options);
    require(bool(r), "expected mapping success; got " + (r ? std::string{} : r.error().message));
    return r.value();
}
double area(const EcadModel& m) {
    require(!m.layers.empty(), "missing copper layer");
    double sum = 0;
    for (const auto& p : m.layers.front().polygons) {
        double a = 0;
        for (std::size_t i = 0; i < p.points.size(); ++i) {
            auto x = p.points[i], y = p.points[(i + 1) % p.points.size()];
            a += double(x[0]) * double(y[1]) - double(y[0]) * double(x[1]);
        }
        sum += a / 2;
    }
    return std::abs(sum);
}
void rejected(const OdbJob& j, const std::vector<std::string>& names, const MapOptions& o = {}) {
    auto r = map_odb(j, o);
    require(!r, "expected rejection");
    require(r.error().message.find("IMPORT_REJECTED") != std::string::npos, "wrong error code");
    for (const auto& n : names)
        require(r.error().message.find(n) != std::string::npos,
                "error missing " + n + ": " + r.error().message);
}
void equal(const EcadModel& a, const EcadModel& b) {
    require(a.id == b.id && a.dbu_m == b.dbu_m && a.outline.points == b.outline.points,
            "model fields");
    require(a.layers.size() == b.layers.size(), "layer count");
    for (std::size_t i = 0; i < a.layers.size(); ++i) {
        const auto& x = a.layers[i];
        const auto& y = b.layers[i];
        require(std::tie(x.name, x.material, x.function, x.thickness_m, x.copper) ==
                    std::tie(y.name, y.material, y.function, y.thickness_m, y.copper),
                "layer fields");
        require(x.polygons.size() == y.polygons.size(), "polygon count");
        for (std::size_t k = 0; k < x.polygons.size(); ++k)
            require(x.polygons[k].points == y.polygons[k].points, "polygon points");
    }
    require(a.report.unsupported.size() == b.report.unsupported.size(), "report count");
    for (std::size_t i = 0; i < a.report.unsupported.size(); ++i) {
        const auto& x = a.report.unsupported[i];
        const auto& y = b.report.unsupported[i];
        require(std::tie(x.code, x.element, x.line) == std::tie(y.code, y.element, y.line),
                "report fields");
    }
    require(a.vias.size() == b.vias.size(), "via count");
    for (std::size_t i = 0; i < a.vias.size(); ++i) {
        const auto& x = a.vias[i];
        const auto& y = b.vias[i];
        require(std::tie(x.id,
                         x.padstack,
                         x.start_layer,
                         x.end_layer,
                         x.position,
                         x.outer_radius_dbu,
                         x.inner_radius_dbu,
                         x.dielectric_layers) == std::tie(y.id,
                                                          y.padstack,
                                                          y.start_layer,
                                                          y.end_layer,
                                                          y.position,
                                                          y.outer_radius_dbu,
                                                          y.inner_radius_dbu,
                                                          y.dielectric_layers),
                "via fields");
    }
    require(a.components.size() == b.components.size(), "component count");
    for (std::size_t i = 0; i < a.components.size(); ++i) {
        const auto& x = a.components[i];
        const auto& y = b.components[i];
        require(std::tie(x.id, x.package, x.position, x.rotation_degrees, x.mirror) ==
                        std::tie(y.id, y.package, y.position, y.rotation_degrees, y.mirror) &&
                    x.outline.points == y.outline.points && x.pads.size() == y.pads.size(),
                "component fields");
        for (std::size_t k = 0; k < x.pads.size(); ++k)
            require(x.pads[k].id == y.pads[k].id && x.pads[k].position == y.pads[k].position,
                    "component pad fields");
    }
    require(a.via_fractions.size() == b.via_fractions.size(), "fraction count");
    for (std::size_t i = 0; i < a.via_fractions.size(); ++i) {
        const auto& x = a.via_fractions[i];
        const auto& y = b.via_fractions[i];
        require(std::tie(x.layer, x.lower, x.upper, x.fraction, x.via_count) ==
                    std::tie(y.layer, y.lower, y.upper, y.fraction, y.via_count),
                "fraction fields");
    }
}
OdbJob missing() {
    auto j = board();
    j.attrs.layer["TOP"].erase(".layer_dielectric");
    j.attrs.layer["CORE"].erase(".layer_dielectric");
    j.attrs.layer["BOTTOM"][".layer_dielectric"] = "0.0";
    return j;
}
void units() {
    auto mm = board(), inch = mm;
    inch.units = OdbUnits::inch;
    for (auto& [name, attrs] : inch.attrs.layer)
        attrs[".layer_dielectric"] = name == "TOP" || name == "BOTTOM" ? "0.0013779527559055118"
                                     : name == "CORE"                  ? "0.031496062992125984"
                                                                       : "0.007874015748031496";
    auto scale = [](OdbContour& c) {
        c.start.x /= 25.4;
        c.start.y /= 25.4;
        for (auto& s : c.segs) {
            s.end.x /= 25.4;
            s.end.y /= 25.4;
            s.center.x /= 25.4;
            s.center.y /= 25.4;
        }
    };
    for (auto& c : inch.profile.contours)
        scale(c);
    for (auto& [name, f] : inch.features)
        for (auto& s : f.surfaces)
            for (auto& c : s.contours)
                scale(c);
    equal(mapped(mm), mapped(inch));
    for (int value : {-1, 2, 127}) {
        auto invalid = mm;
        invalid.units = static_cast<OdbUnits>(value);
        rejected(invalid, {"unsupported ODB units"});
    }
}
void symbol_units() {
    auto mm = board();
    auto& copper = mm.features["TOP"];
    copper.surfaces.clear();
    copper.symbols = {{"round254"}};
    copper.pads = {{{2.54, 2.54}, 0, true}};
    copper.lines = {{{1.27, 5.08}, {3.81, 5.08}, 0, true}};
    copper.arcs = {{{7.62, 5.08}, {5.08, 7.62}, {5.08, 5.08}, 0, true, false}};
    OdbLayer drill;
    drill.name = "DRILL";
    drill.type = "drill";
    drill.start_name = "TOP";
    drill.end_name = "BOTTOM";
    mm.layers.push_back(drill);
    auto& holes = mm.features["DRILL"];
    holes.symbols = {{"round508"}, {"round254"}};
    holes.pads = {{{2.54, 2.54}, 0, true}, {{5.08, 2.54}, 1, true}};
    mm.tools["DRILL"] = {{7, "plated", 0.508}};
    mm.attrs.layer["DRILL"][".plating_thickness"] = "0.0254";
    auto inch = mm;
    inch.units = OdbUnits::inch;
    for (auto& [name, attrs] : inch.attrs.layer)
        attrs[".layer_dielectric"] = name == "TOP" || name == "BOTTOM" ? "0.0013779527559055118"
                                     : name == "CORE"                  ? "0.031496062992125984"
                                                                       : "0.007874015748031496";
    auto scale = [](OdbContour& c) {
        c.start.x /= 25.4;
        c.start.y /= 25.4;
        for (auto& s : c.segs) {
            s.end.x /= 25.4;
            s.end.y /= 25.4;
            s.center.x /= 25.4;
            s.center.y /= 25.4;
        }
    };
    for (auto& c : inch.profile.contours)
        scale(c);
    for (auto& [name, f] : inch.features)
        for (auto& s : f.surfaces)
            for (auto& c : s.contours)
                scale(c);
    auto convert = [](OdbPoint& p) {
        p.x /= 25.4;
        p.y /= 25.4;
    };
    for (auto& [name, f] : inch.features) {
        for (auto& p : f.pads)
            convert(p.at);
        for (auto& l : f.lines) {
            convert(l.s);
            convert(l.e);
        }
        for (auto& a : f.arcs) {
            convert(a.s);
            convert(a.e);
            convert(a.c);
        }
    }
    inch.features["TOP"].symbols = {{"round10"}};
    inch.features["DRILL"].symbols = {{"round20"}, {"round10"}};
    inch.tools["DRILL"].front().finish_size = 0.02;
    inch.attrs.layer["DRILL"][".plating_thickness"] = "0.001";
    const auto mapped_mm = mapped(mm), mapped_inch = mapped(inch);
    auto geometry_mm = mapped_mm, geometry_inch = mapped_inch;
    geometry_mm.vias.clear();
    geometry_inch.vias.clear();
    geometry_mm.via_fractions.clear();
    geometry_inch.via_fractions.clear();
    equal(geometry_mm, geometry_inch);
    require(mapped_mm.vias.size() == mapped_inch.vias.size(), "cross-unit via count");
    for (std::size_t i = 0; i < mapped_mm.vias.size(); ++i) {
        const auto& a = mapped_mm.vias[i];
        const auto& b = mapped_inch.vias[i];
        require(
            std::tie(
                a.id, a.padstack, a.start_layer, a.end_layer, a.position, a.dielectric_layers) ==
                std::tie(
                    b.id, b.padstack, b.start_layer, b.end_layer, b.position, b.dielectric_layers),
            "cross-unit via identity and span");
        near(a.outer_radius_dbu, b.outer_radius_dbu, 1e-12);
        near(a.inner_radius_dbu, b.inner_radius_dbu, 1e-12);
    }
    require(mapped_mm.via_fractions.size() == mapped_inch.via_fractions.size(),
            "cross-unit fraction count");
    for (std::size_t i = 0; i < mapped_mm.via_fractions.size(); ++i) {
        const auto& a = mapped_mm.via_fractions[i];
        const auto& b = mapped_inch.via_fractions[i];
        require(a.layer == b.layer && a.via_count == b.via_count, "cross-unit fraction identity");
        for (std::size_t axis = 0; axis < 2; ++axis) {
            near(a.lower[axis], b.lower[axis], 1e-12);
            near(a.upper[axis], b.upper[axis], 1e-12);
        }
        near(a.fraction, b.fraction, 1e-12);
    }
    require(mapped_inch.vias.size() == 2 && mapped_inch.report.unsupported.empty(),
            "inch tool-based and symbol-based drills map successfully");
    near(mapped_inch.vias[0].outer_radius_dbu, 254);
    near(mapped_inch.vias[0].inner_radius_dbu, 228.6);
    near(mapped_inch.vias[0].outer_radius_dbu - mapped_inch.vias[0].inner_radius_dbu, 25.4, 1e-12);
    near(mapped_inch.vias[1].outer_radius_dbu, 127);
    near(mapped_inch.vias[1].inner_radius_dbu, 101.6);
}
void unavailable_thickness() {
    for (const auto* text :
         {"invalid", "0.035junk", "nan", "inf", "-inf", "1e9999", "", "-1", "0"}) {
        auto j = board();
        j.attrs.layer["TOP"][".layer_dielectric"] = text;
        std::vector<ThicknessRecord> records;
        const auto rejected = map_odb(j, {nullptr, &records});
        require(!rejected && rejected.error().message.find(
                                 "missing thickness for 1 layer(s): TOP") != std::string::npos,
                "unavailable thickness participates in aggregate missing report");
        require(records.size() == 4 && records.front().source == "missing" &&
                    records.front().thickness_m == 0,
                "unavailable ODB thickness records missing provenance");
        ThicknessOverrides overrides{{"TOP", 35e-6}};
        records.clear();
        const auto m = mapped(j, {&overrides, &records});
        near(m.layers.front().thickness_m, 35e-6);
        require(records.front().source == "override" && !records.front().odb_value_m,
                "override fills unavailable ODB thickness without conflict");
    }
}
void line_width() {
    auto j = board();
    auto& f = j.features["TOP"];
    f.surfaces.clear();
    f.symbols = {{"round1000"}};
    f.lines.push_back({{1, 1}, {7, 1}, 0, true});
    near(area(mapped(j)), 6e6 + std::numbers::pi * 250000, 1e-3);
}
void collapsed_line() {
    auto j = board();
    auto& f = j.features["TOP"];
    f.surfaces.clear();
    f.symbols = {{"round1000"}};
    f.lines = {{{1, 1}, {1, 1}, 0, true}};
    near(area(mapped(j)), std::numbers::pi * 250000, 1e-3);
}
void negative() {
    auto j = board();
    auto& f = j.features["TOP"];
    f.symbols = {{"round2000"}};
    f.pads.push_back({{4, 4}, 0, false});
    near(area(mapped(j)), 64e6 - std::numbers::pi * 1e6, 1e-3);
}
// Read only the fixture's S-expression structure, independently of the mapper.
struct KicadNode {
    std::string value;
    std::vector<KicadNode> children;
};
KicadNode kicad_node(std::istream& in) {
    in >> std::ws;
    KicadNode node;
    if (in.peek() == '(') {
        in.get();
        while (in >> std::ws && in.peek() != ')') {
            require(in.peek() != EOF, "unterminated KiCad fixture node");
            node.children.push_back(kicad_node(in));
        }
        require(in.get() == ')', "KiCad closing parenthesis");
    } else if (in.peek() == '"') {
        in.get();
        std::getline(in, node.value, '"');
    } else {
        while (in && in.peek() != EOF && in.peek() != ')' &&
               !std::isspace(static_cast<unsigned char>(in.peek())))
            node.value += static_cast<char>(in.get());
    }
    return node;
}
const KicadNode& child(const KicadNode& node, std::string_view key) {
    for (const auto& c : node.children)
        if (!c.children.empty() && c.children[0].value == key)
            return c;
    throw std::runtime_error("missing KiCad fixture field: " + std::string(key));
}
std::filesystem::path source_root() {
    return std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
}
KicadNode kicad_board() {
    std::ifstream in(source_root() / "tools/fixtures/kicad_odbpp/kicad4l.kicad_pcb");
    require(bool(in), "open KiCad project fixture");
    return kicad_node(in);
}
// The E-ODB-1 parser is a stub on this branch. Read the exported matrix and
// attrlists directly for this mapper test; do not derive ODB inputs from KiCad.
OdbJob kicad_job() {
    const auto root = source_root() / "tests/integration/ecad/data/odbpp/kicad4l";
    std::ifstream matrix(root / "matrix/matrix");
    require(bool(matrix), "open exported matrix");
    OdbJob job;
    job.step = "pcb";
    std::string line;
    OdbLayer layer;
    bool in_layer = false;
    while (std::getline(matrix, line)) {
        std::istringstream fields(line);
        std::string field;
        fields >> field;
        if (field == "LAYER") {
            layer = {};
            in_layer = true;
        } else if (in_layer && field == "}") {
            job.layers.push_back(layer);
            in_layer = false;
        } else if (in_layer) {
            const auto eq = field.find('=');
            const auto key = field.substr(0, eq), value = field.substr(eq + 1);
            if (key == "NAME")
                layer.name = value;
            if (key == "TYPE")
                layer.type = value;
            if (key == "ROW")
                layer.row = std::stoi(value);
            if (key == "START_NAME")
                layer.start_name = value;
            if (key == "END_NAME")
                layer.end_name = value;
        }
    }
    for (const auto& l : job.layers) {
        std::ifstream attrs(root / "steps/pcb/layers" / fold_ascii(l.name) / "attrlist");
        require(bool(attrs), "open exported attrlist: " + l.name);
        while (std::getline(attrs, line)) {
            const auto eq = line.find('=');
            if (eq != std::string::npos && line.starts_with('.'))
                job.attrs.layer[l.name][line.substr(0, eq)] = line.substr(eq + 1);
        }
    }
    require(!job.layers.empty(), "exported matrix has layers");
    return job;
}
void kicad_stackup() {
    const auto pcb = kicad_board();
    const auto& table = child(child(pcb, "setup"), "stackup");
    std::vector<ThicknessRecord> records;
    const auto job = kicad_job();
    const auto raw = mapped(job);
    // Positive exported thickness wins over conflicting project overrides.
    const ThicknessOverrides overrides{
        {"F.CU", 35e-6}, {"IN1.CU", 35e-6}, {"IN2.CU", 35e-6}, {"B.CU", 35e-6}};
    const auto model = mapped(job, {&overrides, &records});
    std::size_t index = 0;
    std::size_t copper_count = 0;
    for (const auto& row : table.children) {
        if (row.children.empty() || row.children[0].value != "layer")
            continue;
        const auto type = child(row, "type").children.at(1).value;
        if (type != "copper" && type != "core" && type != "prepreg")
            continue;
        require(index < model.layers.size(), "missing mapped KiCad stack layer");
        auto name = fold_ascii(row.children.at(1).value);
        std::replace(name.begin(), name.end(), ' ', '_');
        const auto& actual = model.layers[index];
        const bool copper = type == "copper";
        copper_count += copper;
        require(fold_ascii(actual.name) == name && actual.copper == copper,
                "KiCad stack table order and copper flags");
        const double thickness = std::stod(child(row, "thickness").children.at(1).value) * 1e-3;
        near(actual.thickness_m, copper ? 30e-6 : thickness, 1e-6);
        require(index < records.size() && records[index].layer == actual.name &&
                    records[index].source == "odb",
                "KiCad exported thickness provenance");
        if (copper) {
            near(raw.layers[index].thickness_m, 30e-6, 1e-6);
            require(records[index].odb_value_m.has_value(), "copper override conflict provenance");
            near(*records[index].odb_value_m, 30e-6, 1e-12);
        } else {
            near(raw.layers[index].thickness_m, thickness, 1e-6);
            require(!records[index].odb_value_m, "unmodified dielectric provenance");
        }
        near(records[index].thickness_m, copper ? 30e-6 : thickness, 1e-6);
        ++index;
    }
    require(copper_count == 4 && index == 7 && index == model.layers.size() &&
                index == records.size(),
            "complete four-copper-layer KiCad stack table");
}
void kicad_via() {
    auto job = kicad_job();
    const std::string drill = "DRILL_PLATED_F.CU-B.CU";
    const auto file = source_root() / "tests/integration/ecad/data/odbpp/kicad4l/steps/pcb/layers" /
                      fold_ascii(drill) / "features";
    std::ifstream in(file);
    require(bool(in), "open exported drill features");
    auto& features = job.features[drill];
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream fields(line);
        std::string tag;
        fields >> tag;
        if (tag.starts_with('$')) {
            int index = std::stoi(tag.substr(1));
            std::string symbol;
            fields >> symbol;
            // KiCad's r alias is outside this mapper's round/rect/oval contract.
            if (symbol.size() > 1 && symbol[0] == 'r' && symbol[1] >= '0' && symbol[1] <= '9')
                symbol = "round" + symbol.substr(1);
            require(index == static_cast<int>(features.symbols.size()), "fixture symbol order");
            features.symbols.push_back({symbol});
        } else if (tag == "P") {
            OdbPad pad;
            fields >> pad.at.x >> pad.at.y >> pad.sym;
            require(bool(fields), "exported drill pad fields");
            features.pads.push_back(pad);
        }
    }
    // KiCad omits plating thickness; supply it explicitly, never as a production default.
    job.attrs.layer[drill][".plating_thickness"] = "0.025";
    const auto model = mapped(job);
    const auto pcb = kicad_board();
    const auto& footprint = child(pcb, "footprint");
    const auto& origin = child(footprint, "at");
    std::size_t count = 0;
    for (const auto& pad : footprint.children) {
        if (pad.children.empty() || pad.children[0].value != "pad")
            continue;
        const auto& hole = child(pad, "drill");
        if (hole.children.at(1).value == "oval")
            continue;
        require(count < model.vias.size(), "missing KiCad circular hole");
        const auto& at = child(pad, "at");
        const auto& actual = model.vias[count++];
        const double x =
            std::stod(origin.children.at(1).value) + std::stod(at.children.at(1).value);
        const double y =
            std::stod(origin.children.at(2).value) + std::stod(at.children.at(2).value);
        require(std::abs(actual.position[0] - x * 1000) <= 1 &&
                    std::abs(actual.position[1] + y * 1000) <= 1,
                "KiCad hole position (ODB Y inverted)");
        require(std::abs(actual.outer_radius_dbu - std::stod(hole.children.at(1).value) * 500) <= 1,
                "KiCad hole outer radius");
        require(actual.start_layer == "F.CU" && actual.end_layer == "B.CU",
                "KiCad through-hole span");
    }
    require(count == 1 && count == model.vias.size(), "all KiCad circular holes mapped");
    require(std::any_of(model.report.unsupported.begin(),
                        model.report.unsupported.end(),
                        [&](const auto& item) { return item.element == "Slot:" + drill; }),
            "KiCad oval drill remains unsupported");
}
void stackup() {
    kicad_stackup();
    std::vector<ThicknessRecord> records;
    auto m = mapped(board(), {nullptr, &records});
    require(records.size() == 4, "one thickness record per stack layer");
    require(m.layers.size() == 4, "four stack layers");
    const char* names[] = {"TOP", "CORE", "BOTTOM", "PREPREG"};
    double t[] = {35e-6, 800e-6, 35e-6, 200e-6};
    for (std::size_t i = 0; i < 4; ++i) {
        require(m.layers[i].name == names[i] && m.layers[i].copper == (i % 2 == 0),
                "stack order/flags");
        near(m.layers[i].thickness_m, t[i]);
        require(records[i].layer == names[i] && records[i].source == "odb",
                "ODB thickness provenance");
        near(records[i].thickness_m, t[i]);
        require(m.layers[i].material == (i % 2 == 0 ? "copper" : "sio2"), "material");
    }
}
void missing_material() {
    auto j = board();
    j.attrs.layer["CORE"].erase(".material");
    require(mapped(j).layers[1].material.empty(), "missing material is preserved without defaults");
    j.attrs.layer["CORE"][".material"] = "";
    require(mapped(j).layers[1].material.empty(), "empty material is preserved without defaults");
    j.attrs.layer.erase("CORE");
    std::vector<ThicknessRecord> records;
    rejected(j, {"missing thickness", "CORE"}, {nullptr, &records});
    require(records.size() == 4, "missing attrlist still records every layer");
    ThicknessOverrides overrides{{"CORE", 800e-6}};
    records.clear();
    const auto m = mapped(j, {&overrides, &records});
    require(m.layers[1].material.empty(), "absent attrlist does not invent a material");
    require(records.size() == 4 && records[1].source == "override",
            "thickness override allows absent attrlist without defaulting material");
    near(m.layers[1].thickness_m, 800e-6);
}
void oval_capsules() {
    for (const auto& [symbol, vertical, circular] : {std::tuple{"oval4000x2000", false, false},
                                                     std::tuple{"oval2000x4000", true, false},
                                                     std::tuple{"oval2000x2000", false, true}}) {
        for (double rotation : {0.0, 90.0, 37.0}) {
            for (bool mirror : {false, true}) {
                auto j = board();
                auto& f = j.features["TOP"];
                f.surfaces.clear();
                f.symbols = {{symbol}};
                OdbPad pad;
                pad.at = {5, 5};
                pad.sym = 0;
                pad.positive = true;
                pad.rot_deg = rotation;
                pad.mirror = mirror;
                f.pads = {pad};
                const auto m = mapped(j);
                require(std::abs(area(m) - ((circular ? 0 : 4e6) + std::numbers::pi * 1e6)) <=
                            ((circular ? 0 : 4e6) + std::numbers::pi * 1e6) * 2e-4,
                        std::string(symbol) + " rotation " + std::to_string(rotation) + " area " +
                            std::to_string(area(m)));
                require(m.layers.front().polygons.size() == 1, "one capsule contour");
                const auto& points = m.layers.front().polygons.front().points;
                long double signed_area = 0;
                for (std::size_t i = 0; i < points.size(); ++i) {
                    const auto& next = points[(i + 1) % points.size()];
                    signed_area += static_cast<long double>(points[i][0]) * next[1] -
                                   static_cast<long double>(next[0]) * points[i][1];
                }
                require(signed_area > 0, "placed mirrored pad has positive exterior winding");
                const double a = rotation * std::numbers::pi / 180;
                int straight_sides = 0;
                for (std::size_t i = 0; i < points.size(); ++i) {
                    const double dx = double(points[i][0]) - 5000;
                    const double dy = double(points[i][1]) - 5000;
                    double x = (dx * std::cos(a) + dy * std::sin(a)) * (mirror ? -1 : 1);
                    double y = -dx * std::sin(a) + dy * std::cos(a);
                    if (vertical)
                        std::swap(x, y);
                    const double offset = circular ? 0 : 1000;
                    const double distance = std::hypot(std::max(0.0, std::abs(x) - offset), y);
                    require(std::abs(distance - 1000) <= 1.5, "capsule boundary distance");
                    const auto& next = points[(i + 1) % points.size()];
                    if (std::abs(std::hypot(double(next[0] - points[i][0]),
                                            double(next[1] - points[i][1])) -
                                 2000) <= 1.5)
                        ++straight_sides;
                }
                require(straight_sides == (circular ? 0 : 2), "capsule straight sides");
            }
        }
    }
}
void missing_thickness() {
    std::vector<ThicknessRecord> records;
    rejected(missing(), {"3", "TOP", "CORE", "BOTTOM"}, {nullptr, &records});
    require(records.size() == 4, "missing thickness alone records every layer");
    records.clear();
    auto j = missing();
    j.attrs.layer["CORE"].erase(".material");
    j.attrs.layer["PREPREG"][".material"] = "";
    rejected(j, {"missing thickness for 3 layer(s): TOP, CORE, BOTTOM"}, {nullptr, &records});
    require(records.size() == 4, "rejected import still records every layer");
    const char* names[] = {"TOP", "CORE", "BOTTOM", "PREPREG"};
    for (std::size_t i = 0; i < records.size(); ++i) {
        require(records[i].layer == names[i], "missing thickness record layer");
        require(records[i].source == (i < 3 ? "missing" : "odb"), "missing provenance");
        require(!records[i].odb_value_m, "missing thickness has no conflict");
        if (i < 3)
            require(records[i].thickness_m == 0, "missing thickness is explicitly zero");
        else
            near(records[i].thickness_m, 200e-6);
    }
}
void overrides() {
    ThicknessOverrides o = {
        {"TOP", 35e-6}, {"CORE", 800e-6}, {"BOTTOM", 35e-6}, {"PREPREG", 300e-6}};
    std::vector<ThicknessRecord> records;
    auto m = mapped(missing(), {&o, &records});
    require(records.size() == 4, "one record per layer");
    for (std::size_t i = 0; i < records.size(); ++i) {
        const auto& r = records[i];
        require(r.layer == m.layers[i].name, "record layer");
        near(r.thickness_m, i == 3 ? 200e-6 : o.at(r.layer));
        near(m.layers[i].thickness_m, r.thickness_m);
        require(r.source == (i == 3 ? "odb" : "override"), "first available thickness source");
        if (i == 3) {
            require(r.odb_value_m.has_value(), "ODB override conflict provenance");
            near(*r.odb_value_m, 200e-6, 1e-12);
        } else
            require(!r.odb_value_m, "missing ODB has no conflict provenance");
    }
}
void override_provenance() {
    auto j = board();
    const double odb = mapped(j).layers.front().thickness_m;
    for (const auto& [value, conflict] : {std::pair{2 * odb, true},
                                          {odb * (1 + 2e-9), true},
                                          {odb * (1 - 2e-9), true},
                                          {odb * (1 + 0.5e-9), false},
                                          {odb * (1 - 0.5e-9), false},
                                          {odb, false}}) {
        ThicknessOverrides o{{"top", value}};
        std::vector<ThicknessRecord> records;
        const auto m = mapped(j, {&o, &records});
        near(m.layers.front().thickness_m, odb, 1e-12);
        near(records.front().thickness_m, odb, 1e-12);
        require(records.front().source == "odb", "positive ODB wins over every override");
        require(records.front().odb_value_m.has_value() == conflict,
                "relative conflict threshold provenance");
        if (conflict)
            near(*records.front().odb_value_m, odb, 1e-12);
        require(!records.back().odb_value_m, "no matching override has no conflict provenance");
    }
    for (double value : {0.0,
                         -1.0,
                         std::numeric_limits<double>::infinity(),
                         std::numeric_limits<double>::quiet_NaN()}) {
        ThicknessOverrides o{{"TOP", value}};
        near(mapped(j, {&o, nullptr}).layers.front().thickness_m, odb, 1e-12);
        auto missing_job = j;
        missing_job.attrs.layer["TOP"][".layer_dielectric"] = "0";
        rejected(missing_job, {"invalid override thickness", "TOP"}, {&o, nullptr});
    }
    ThicknessOverrides o{{"TOP", 70e-6}};
    near(mapped(j, {&o, nullptr}).layers.front().thickness_m, odb, 1e-12);
}

void weight() {
    auto j = board();
    j.attrs.layer["TOP"][".layer_dielectric"] = "0";
    j.attrs.layer["TOP"][".copper_weight"] = "1.0";
    rejected(j, {"TOP"});
    ThicknessOverrides o = {{"TOP", 35e-6}};
    std::vector<ThicknessRecord> records;
    mapped(j, {&o, &records});
    j.attrs.layer["TOP"][".layer_dielectric"] = "-0.01";
    near(mapped(j, {&o, nullptr}).layers.front().thickness_m, 35e-6);
    require(records.size() == 4 && records[0].source == "override", "weight override source");
}
void override_case() {
    ThicknessOverrides o = {{"top", 35e-6}, {"core", 800e-6}, {"bottom", 35e-6}, {"unused", 1e-3}};
    auto m = mapped(missing(), {&o, nullptr});
    near(m.layers[0].thickness_m, 35e-6);
}
void skip() {
    auto j = board();
    const char* types[] = {"silk_screen", "solder_paste", "solder_mask", "document", "component"};
    std::set<std::string> expected;
    for (int i = 0; i < 5; ++i) {
        OdbLayer l;
        l.name = "skip" + std::to_string(i);
        l.type = types[i];
        l.row = i + 5;
        j.layers.push_back(l);
        expected.insert(i == 2 ? "SolderMask:" + l.name : "SkippedLayer:" + l.name + ":" + l.type);
    }
    auto m = mapped(j);
    require(m.layers.size() == 4, "excluded layers");
    std::set<std::string> actual;
    for (const auto& u : m.report.unsupported)
        actual.insert(u.element);
    require(actual == expected, "skip report");
}
void unsupported() {
    for (const auto* name : {"unknown_symbol",
                             "r1000",
                             "s1000",
                             "R1000",
                             "S1000",
                             "roundfoo",
                             "roundfoo1000",
                             "rectfoo1000x2000",
                             "ovalfoo1000x2000",
                             "ROUNDfoo1000",
                             "round1000foo",
                             "rect1000x2000foo",
                             "oval1000x2000foo"}) {
        auto j = board();
        auto& f = j.features["TOP"];
        f.symbols = {{name}};
        f.pads.push_back({{4, 4}, 0, true});
        auto m = mapped(j);
        require(m.report.unsupported.size() == 1, "one unknown symbol report");
        require(m.report.unsupported.front().element == "Symbol:TOP:0", "unknown symbol identity");
        near(area(m), 64e6, 1e-3);
    }
}
void stroke_symbols() {
    for (const auto* name : {"oval2000x1000", "rect2000x1000", "round2000x1000"}) {
        auto j = board();
        auto& f = j.features["TOP"];
        f.symbols = {{name}};
        f.lines = {{{1, 1}, {7, 1}, 0, true}};
        f.arcs = {{{2, 0}, {0, 2}, {0, 0}, 0, true, false}};
        const auto m = mapped(j);
        near(area(m), 64e6, 1e-3);
        require(m.report.unsupported.size() == 2, "incompatible line and arc symbols rejected");
        for (const auto& u : m.report.unsupported)
            require(u.element == "Symbol:TOP:0", "unsupported stroke symbol identity");
    }
    auto j = board();
    auto& f = j.features["TOP"];
    f.surfaces.clear();
    f.symbols = {{"round1000"}};
    f.lines = {{{1, 1}, {7, 1}, 0, true}};
    f.arcs = {{{2, 0}, {0, 2}, {0, 0}, 0, true, false}};
    const auto reference = mapped(j);
    f.symbols = {{"oval1000x1000"}};
    equal(reference, mapped(j));
}
void unknown_drill_symbols() {
    for (bool conflicting_tools : {false, true}) {
        auto j = board();
        OdbLayer drill;
        drill.name = "DRILL";
        drill.type = "drill";
        drill.start_name = "TOP";
        drill.end_name = "BOTTOM";
        j.layers.push_back(drill);
        auto& f = j.features["DRILL"];
        f.symbols = {{"unknown"}};
        f.pads = {{{2, 3}, 0, true}, {{4, 5}, 1, true}};
        if (conflicting_tools)
            j.tools["DRILL"] = {{1, "plated", 0}, {2, "slot", 0.0001}};
        const auto m = mapped(j);
        require(m.vias.empty() && m.via_fractions.empty(), "unknown drill symbols produce no vias");
        require(m.report.unsupported.size() == 2, "one report per unknown drill symbol");
        for (std::size_t i = 0; i < 2; ++i)
            require(m.report.unsupported[i].element == "Symbol:DRILL:" + std::to_string(i),
                    "unknown drill symbol precedes tool classification");
    }
}
void via() {
    kicad_via();
    auto j = board();
    OdbLayer l;
    l.name = "DRILL";
    l.type = "drill";
    l.row = 5;
    l.start_name = "TOP";
    l.end_name = "BOTTOM";
    j.layers.push_back(l);
    j.attrs.layer["DRILL"][".plating_thickness"] = "0.025";
    j.tools["DRILL"] = {{1, "plated", 0.6}, {2, "slot", 0.8}};
    auto& f = j.features["DRILL"];
    f.symbols = {{"round600"}, {"round800"}};
    f.pads = {{{2, 3}, 0, true}, {{6, 7}, 0, true}, {{8, 8}, 1, true}};
    f.lines.push_back({{4, 4}, {5, 4}, 1, true});
    auto m = mapped(j);
    require(m.vias.size() == 2, "two circular holes");
    for (std::size_t i = 0; i < 2; ++i) {
        const auto& v = m.vias[i];
        require(std::abs(v.position[0] - (i == 0 ? 2000 : 6000)) <= 1 &&
                    std::abs(v.position[1] - (i == 0 ? 3000 : 7000)) <= 1,
                "via position");
        require(std::abs(v.outer_radius_dbu - 300) <= 1, "via radius");
        require(v.start_layer == "TOP" && v.end_layer == "BOTTOM", "via span");
    }
    bool slot_reported = false;
    for (const auto& u : m.report.unsupported)
        slot_reported = slot_reported || u.element == "Slot:DRILL";
    require(slot_reported && m.report.unsupported.size() == 2, "line and tool slot unsupported");
    require(!m.via_fractions.empty(), "via rasterization initialized");
    // Negative drill pads must be ignored before symbol or tool interpretation.
    f.pads.push_back({{100, 100}, 0, false});
    f.pads.push_back({{100, 100}, 1, false});
    f.pads.push_back({{100, 100}, 99, false});
    equal(m, mapped(j));
    f.lines.clear();
    std::erase_if(f.pads, [](const auto& p) { return p.positive; });
    const auto negative_only = mapped(j);
    require(negative_only.vias.empty() && negative_only.via_fractions.empty() &&
                negative_only.report.unsupported.empty(),
            "negative-only drills produce no vias, fraction cells, or diagnostics");
}
OdbJob via_acceptance_board() {
    auto j = board();
    OdbLayer drill;
    drill.name = "DRILL";
    drill.type = "drill";
    drill.row = 5;
    drill.start_name = "TOP";
    drill.end_name = "BOTTOM";
    j.layers.push_back(drill);
    j.attrs.layer["DRILL"][".plating_thickness"] = "0.025";
    j.tools["DRILL"] = {{1, "plated", 0.6}, {2, "slot", 0.8}};
    auto& f = j.features["DRILL"];
    f.symbols = {{"round600"}, {"round800"}};
    f.pads = {{{2, 3}, 0, true}, {{6, 7}, 0, true}, {{8, 8}, 1, true}};
    f.lines.push_back({{4, 4}, {5, 4}, 1, true});
    return j;
}
void drill_diagnostics(const EcadModel& m, std::initializer_list<std::string_view> expected) {
    require(m.report.unsupported.size() == expected.size(), "drill diagnostic count");
    std::size_t i = 0;
    for (const auto element : expected)
        require(m.report.unsupported[i++].element == element, "drill diagnostic emission order");
}
void via_tool_type_via() {
    auto j = via_acceptance_board();
    const auto reference = mapped(j);
    require(reference.vias.size() == 2, "PLATED reference has two circular holes");
    require(!reference.via_fractions.empty(), "PLATED reference has via fractions");
    drill_diagnostics(reference, {"Slot:DRILL", "Slot:DRILL"});
    for (std::size_t i = 0; i < reference.vias.size(); ++i) {
        const auto& v = reference.vias[i];
        require(std::abs(v.position[0] - (i == 0 ? 2000 : 6000)) <= 1 &&
                    std::abs(v.position[1] - (i == 0 ? 3000 : 7000)) <= 1,
                "reference via position within one DBU");
        near(v.outer_radius_dbu, 300, 1e-9);
        near(v.inner_radius_dbu, 275, 1e-9);
        require(v.start_layer == "TOP" && v.end_layer == "BOTTOM", "reference via span");
        require(v.dielectric_layers == std::vector<std::size_t>{1}, "reference dielectric index");
    }
    for (const auto* type : {"VIA", "via", "Via"}) {
        j.tools["DRILL"] = {{1, type, 0.6}, {2, "slot", 0.8}};
        const auto actual = mapped(j);
        require(actual.vias.size() == 2, std::string(type) + " must map two plated holes");
        equal(reference, actual);
    }
    j.tools["DRILL"] = {{1, "plated", 0.6}, {2, "VIA", 0.6}, {3, "slot", 0.8}};
    for (int order = 0; order < 2; ++order) {
        equal(reference, mapped(j));
        std::reverse(j.tools["DRILL"].begin(), j.tools["DRILL"].end());
    }
    for (const auto* conflict : {"NON_PLATED", "slot"}) {
        j.tools["DRILL"] = {{1, "VIA", 0.6}, {2, conflict, 0.6}, {3, "slot", 0.8}};
        for (int order = 0; order < 2; ++order) {
            const auto actual = mapped(j);
            require(actual.vias.empty() && actual.via_fractions.empty(),
                    "ambiguous VIA tools produce no vias or fractions");
            drill_diagnostics(
                actual,
                {"Slot:DRILL", "ViaToolAmbiguous:DRILL", "ViaToolAmbiguous:DRILL", "Slot:DRILL"});
            std::reverse(j.tools["DRILL"].begin(), j.tools["DRILL"].end());
        }
    }
}
void via_span_case() {
    const auto reference_job = via_acceptance_board();
    const auto reference = mapped(reference_job);
    require(reference.vias.size() == 2, "case-sensitive reference has two vias");
    drill_diagnostics(reference, {"Slot:DRILL", "Slot:DRILL"});
    for (const auto& [start, end] : {std::pair{"top", "bottom"}, std::pair{"Top", "BOTTOM"}}) {
        auto j = reference_job;
        j.layers.back().start_name = start;
        j.layers.back().end_name = end;
        const auto actual = mapped(j);
        require(actual.vias.size() == 2, "case-insensitive span must map two vias");
        for (const auto& v : actual.vias)
            require(v.start_layer == "TOP" && v.end_layer == "BOTTOM",
                    "span uses original model layer names");
        equal(reference, actual);
    }
    for (const auto* name : {"top", "bottom"}) {
        auto j = reference_job;
        OdbLayer collision;
        collision.name = name;
        collision.type = "signal";
        collision.row = 6;
        j.layers.push_back(collision);
        j.attrs.layer[name][".layer_dielectric"] = "0.035";
        const auto actual = mapped(j);
        require(actual.layers.size() == reference.layers.size() + 1,
                "folded collision reaches drill mapping with a valid additional layer");
        require(actual.vias.empty() && actual.via_fractions.empty(),
                "folded duplicate span endpoint produces no vias or fractions");
        drill_diagnostics(actual, {"Slot:DRILL", "ViaSpan:DRILL", "ViaSpan:DRILL", "Slot:DRILL"});
    }
}
void via_region_initialization() {
    auto j = board();
    OdbLayer drill;
    drill.name = "DRILL";
    drill.type = "drill";
    drill.start_name = "TOP";
    drill.end_name = "BOTTOM";
    j.layers.push_back(drill);
    j.attrs.layer["DRILL"][".plating_thickness"] = "0.025";
    auto& f = j.features["DRILL"];
    f.symbols = {{"round600"}};
    for (bool overlap : {false, true}) {
        f.pads = {{{2, 3}, 0, true}, {{overlap ? 2.2 : 200, 3}, 0, true}};
        if (overlap)
            f.pads.push_back(f.pads.front()); // duplicate bounds must not duplicate cells
        const auto m = mapped(j);
        ViaFraction reference;
        reference.layer = 1;
        reference.lower = {1700, 2700};
        reference.upper = {overlap ? 2500.0 : 200300.0, 3300};
        std::vector<ViaFraction> cells{reference};
        require(rasterize_vias(m, cells), "IPC envelope reference rasterizes");
        require(m.via_fractions.size() == cells.size(), "one IPC envelope for all layer vias");
        for (std::size_t n = 0; n < cells.size(); ++n)
            require(m.via_fractions[n].lower == cells[n].lower &&
                        m.via_fractions[n].upper == cells[n].upper &&
                        m.via_fractions[n].fraction == cells[n].fraction &&
                        m.via_fractions[n].via_count == cells[n].via_count,
                    "layer envelope bounds and coverage");
        const auto& actual = m.via_fractions.front();
        require(actual.layer == cells[0].layer && actual.lower == cells[0].lower &&
                    actual.upper == cells[0].upper && actual.via_count == cells[0].via_count &&
                    actual.fraction == cells[0].fraction,
                "IPC envelope bounds and rasterization match exactly");
    }
}
// Compare exposed cells with the real IPC importer, including layer order and coverage.
void ipc_fraction_parity(const EcadModel& odb) {
    std::random_device random;
    const auto dir =
        std::filesystem::temp_directory_path() / ("odb-ipc-vias-" + std::to_string(random()));
    require(std::filesystem::create_directory(dir), "private IPC via fixture directory");
    struct Cleanup {
        std::filesystem::path dir;
        ~Cleanup() { std::filesystem::remove_all(dir); }
    } cleanup{dir};
    const auto file = dir / "vias.xml";
    {
        std::ofstream out(file);
        out << "<IPC-2581><CadHeader units='MICRON'/><CadData>";
        for (const auto& layer : odb.layers)
            if (layer.copper)
                out << "<Layer name='" << layer.name << "' layerFunction='SIGNAL'/>";
        out << "<Stackup><StackupGroup>";
        for (const auto& layer : odb.layers)
            out << "<StackupLayer layerOrGroupRef='" << layer.name << "' thickness='"
                << layer.thickness_m * 1e6 << "'/>";
        out << "</StackupGroup></Stackup><Step name='board'>";
        for (std::size_t i = 0; i < odb.vias.size(); ++i) {
            const auto& v = odb.vias[i];
            out << "<LayerFeature layerRef='drill" << i << "'><Set><Hole name='via" << i
                << "' diameter='" << 2 * v.outer_radius_dbu << "' platingStatus='PLATED' x='"
                << v.position[0] << "' y='" << v.position[1] << "'><SpecRef id='wall" << i
                << "'/></Hole></Set></LayerFeature>";
        }
        out << "</Step>";
        for (std::size_t i = 0; i < odb.vias.size(); ++i) {
            const auto& v = odb.vias[i];
            out << "<Layer name='drill" << i << "' layerFunction='DRILL'><Span fromLayer='"
                << v.start_layer << "' toLayer='" << v.end_layer << "'/></Layer>"
                << "<Spec name='wall" << i
                << "'><General type='MATERIAL'><Property name='PLATING_THICKNESS' value='"
                << v.outer_radius_dbu - v.inner_radius_dbu << "' unit='MICRON'/></General></Spec>";
        }
        out << "</CadData></IPC-2581>";
    }
    const auto ipc = read_ipc2581(file);
    require(bool(ipc), "IPC via reference imports: " + ipc.error);
    require(ipc.model.report.unsupported.empty() && ipc.model.vias.size() == odb.vias.size(),
            "IPC via reference has matching supported vias");
    const auto& ipc_cells = ipc.model.via_fractions;
    require(ipc_cells.size() == odb.via_fractions.size(), "IPC importer cell count");
    for (std::size_t i = 0; i < odb.via_fractions.size(); ++i) {
        const auto& a = odb.via_fractions[i];
        const auto& b = ipc_cells[i];
        require(std::tie(a.layer, a.lower, a.upper, a.fraction, a.via_count) ==
                    std::tie(b.layer, b.lower, b.upper, b.fraction, b.via_count),
                "exact IPC fraction bounds, order, coverage and count");
    }
}
void via_layer_envelope() {
    auto j = board();
    OdbLayer drill;
    drill.name = "DRILL";
    drill.type = "drill";
    drill.start_name = "TOP";
    drill.end_name = "BOTTOM";
    j.layers.push_back(drill);
    j.attrs.layer["DRILL"][".plating_thickness"] = "0.025";
    auto& f = j.features["DRILL"];
    f.symbols = {{"round600"}};
    // The middle disk connects the first two only transitively.
    const std::array<OdbPad, 3> pads{{{{2, 3}, 0, true}, {{3, 3}, 0, true}, {{2.5, 3}, 0, true}}};
    std::array<int, 3> order{0, 1, 2};
    do {
        f.pads.clear();
        for (int index : order)
            f.pads.push_back(pads[index]);
        const auto m = mapped(j);
        ipc_fraction_parity(m);
        require(m.via_fractions.size() == 1, "IPC layer envelope contains all vias");
        const auto& cell = m.via_fractions.front();
        require(cell.lower == std::array<double, 2>{1700, 2700} &&
                    cell.upper == std::array<double, 2>{3300, 3300} && cell.via_count == 3,
                "IPC layer envelope bounds and membership");
    } while (std::next_permutation(order.begin(), order.end()));
    f.pads = {{{2, 3}, 0, true}, {{2.5, 3.5}, 0, true}};
    const auto m = mapped(j);
    ipc_fraction_parity(m);
    ViaFraction reference;
    reference.layer = 1;
    reference.lower = {1700, 2700};
    reference.upper = {2800, 3800};
    std::vector<ViaFraction> cells{reference};
    require(rasterize_vias(m, cells), "disjoint diagonal reference rasterizes");
    require(m.via_fractions.size() == 1, "disjoint diagonal disks share layer envelope");
    for (std::size_t i = 0; i < cells.size(); ++i)
        require(m.via_fractions[i].lower == cells[i].lower &&
                    m.via_fractions[i].upper == cells[i].upper &&
                    m.via_fractions[i].fraction == cells[i].fraction,
                "diagonal local bounds and coverage");
}

void via_fraction_invariants() {
    auto j = board();
    // Cross both dielectrics so exposed ordering is checked on multiple cells.
    auto bottom = j.layers[2];
    bottom.name = "LAST";
    bottom.row = 5;
    j.layers.push_back(bottom);
    j.attrs.layer["LAST"] = j.attrs.layer["BOTTOM"];
    OdbLayer drill;
    drill.name = "DRILL";
    drill.type = "drill";
    drill.start_name = "TOP";
    drill.end_name = "LAST";
    j.layers.push_back(drill);
    j.attrs.layer["DRILL"][".plating_thickness"] = "0.025";
    auto& f = j.features["DRILL"];
    f.symbols = {{"round600"}};
    for (double separation : {0.0, 0.2, 0.6, 0.7}) {
        const std::array<OdbPad, 3> pads{
            {{{2, 3}, 0, true}, {{2 + separation, 3}, 0, true}, {{2, 3}, 0, true}}};
        std::array<int, 3> order{0, 1, 2};
        std::vector<ViaFraction> reference;
        do {
            f.pads.clear();
            for (int index : order)
                f.pads.push_back(pads[index]);
            const auto m = mapped(j);
            ipc_fraction_parity(m);
            const auto& cells = m.via_fractions;
            auto key = [](const ViaFraction& c) { return std::tie(c.layer, c.lower, c.upper); };
            require(cells.size() == 2, "one envelope per dielectric");
            for (std::size_t i = 0; i < cells.size(); ++i) {
                const auto& c = cells[i];
                for (std::size_t axis = 0; axis < 2; ++axis)
                    require(std::isfinite(c.lower[axis]) && std::isfinite(c.upper[axis]) &&
                                c.lower[axis] < c.upper[axis],
                            "finite strict fraction bounds");
                require(std::isfinite(c.fraction) && c.fraction > 0 && c.fraction <= 1,
                        "finite fraction coverage");
                require(i == 0 || key(cells[i - 1]) < key(c),
                        "canonical order without duplicate cells");
            }
            if (reference.empty())
                reference = cells;
            require(cells.size() == reference.size(), "permutation cell count");
            for (std::size_t i = 0; i < cells.size(); ++i) {
                require(key(cells[i]) == key(reference[i]) &&
                            cells[i].via_count == reference[i].via_count,
                        "permutation exposed bounds, order and via count");
                near(cells[i].fraction, reference[i].fraction, 1e-12);
            }
        } while (std::next_permutation(order.begin(), order.end()));
    }
}
void mixed_tools() {
    auto j = board();
    OdbLayer drill;
    drill.name = "DRILL";
    drill.type = "drill";
    drill.start_name = "TOP";
    drill.end_name = "BOTTOM";
    j.layers.push_back(drill);
    j.attrs.layer["DRILL"][".plating_thickness"] = "0.025";
    auto& f = j.features["DRILL"];
    f.symbols = {{"round600"}};
    f.pads = {{{2, 3}, 0, true}, {{6, 7}, 0, true}};
    // Repeated plated sizes retain the matrix span and symbol diameter.
    j.tools["DRILL"] = {{7, "plated", 0.6}, {9, "PLATED", 0.6}};
    for (int order = 0; order < 2; ++order) {
        const auto m = mapped(j);
        require(m.vias.size() == 2 && !m.via_fractions.empty() && m.report.unsupported.empty(),
                "same-classification duplicate sizes map both vias");
        for (std::size_t i = 0; i < m.vias.size(); ++i) {
            const auto& v = m.vias[i];
            require(v.start_layer == "TOP" && v.end_layer == "BOTTOM", "matrix drill span");
            require(v.position ==
                        std::array<int64_t, 2>{i == 0 ? 2000 : 6000, i == 0 ? 3000 : 7000},
                    "duplicate size via position");
            near(v.outer_radius_dbu, 300);
            near(v.inner_radius_dbu, 275);
        }
        std::reverse(j.tools["DRILL"].begin(), j.tools["DRILL"].end());
    }
    j.tools["DRILL"] = {{7, "plated", 0.6005}, {9, "plated", 0.5995}};
    const auto rounded = mapped(j);
    require(rounded.vias.size() == 2 && rounded.report.unsupported.empty(),
            "rounded tool matching retains rounded symbol diameter");
    for (const auto& v : rounded.vias) {
        near(v.outer_radius_dbu, 300);
        near(v.inner_radius_dbu, 275);
    }
    for (const auto* type : {"non-plated", "slot"}) {
        f.symbols = {{"round600.49"}};
        j.tools["DRILL"] = {{7, "plated", 0.60049}, {9, type, 0.60051}};
        const auto m = mapped(j);
        require(m.vias.size() == 2 && m.report.unsupported.empty(),
                "different rounded DBU diameters cannot create classification ambiguity");
        near(m.vias.front().outer_radius_dbu, 300);
        f.symbols = {{"round600.51"}};
        j.tools["DRILL"] = {{7, "plated", 0.60051}};
        j.attrs.layer["DRILL"][".plating_thickness"] = "0.0256";
        const auto odd = mapped(j);
        near(odd.vias.front().outer_radius_dbu, 300.5);
        near(odd.vias.front().inner_radius_dbu, 274.9);
        f.symbols = {{"round600"}};
        j.attrs.layer["DRILL"][".plating_thickness"] = "0.025";
    }
    for (const auto& type : {"non-plated", "slot"}) {
        j.tools["DRILL"] = {{7, "plated", 0.6}, {9, type, 0.6}};
        for (int order = 0; order < 2; ++order) {
            const auto m = mapped(j);
            require(m.vias.empty() && m.via_fractions.empty() && m.report.unsupported.size() == 2,
                    "conflicting tool classifications cannot classify either hole");
            for (const auto& item : m.report.unsupported)
                require(item.element == "ViaToolAmbiguous:DRILL",
                        "unlinked ambiguous hole is reported instead of guessed");
            std::reverse(j.tools["DRILL"].begin(), j.tools["DRILL"].end());
        }
    }
    // Distinct diameters remain unambiguous in the frozen representation.
    f.symbols.push_back({"round800"});
    f.pads.back().sym = 1;
    j.tools["DRILL"] = {{7, "plated", 0.6}, {9, "non-plated", 0.8}};
    for (int order = 0; order < 2; ++order) {
        const auto m = mapped(j);
        require(m.vias.size() == 1 &&
                    m.vias.front().position == std::array<int64_t, 2>{2000, 3000} &&
                    m.report.unsupported.empty(),
                "distinct diameters classify plated and non-plated holes in either tool order");
        near(m.vias.front().inner_radius_dbu, 275);
        std::reverse(j.tools["DRILL"].begin(), j.tools["DRILL"].end());
    }
}
void non_plated() {
    auto j = board();
    OdbLayer drill;
    drill.name = "DRILL";
    drill.type = "drill";
    drill.start_name = "TOP";
    drill.end_name = "BOTTOM";
    j.layers.push_back(drill);
    auto& f = j.features["DRILL"];
    f.symbols = {{"round600"}};
    f.pads = {{{2, 3}, 0, true}};
    for (const auto* type : {"NON-PLATED", "non_plated", "NonPlated"}) {
        j.tools["DRILL"] = {{1, type, 0.6}};
        for (bool plating : {false, true}) {
            if (plating)
                j.attrs.layer["DRILL"][".plating_thickness"] = "0.025";
            else
                j.attrs.layer.erase("DRILL");
            const auto m = mapped(j);
            require(m.vias.empty() && m.via_fractions.empty() && m.report.unsupported.empty(),
                    "non-plated holes produce neither vias nor missing-plating reports");
        }
    }
}
void duplicate_geometry() {
    auto j = board();
    j.features["TOP"].surfaces.push_back(j.features["TOP"].surfaces.front());
    near(area(mapped(j)), 64e6, 1e-3);
}
void duplicate_layer() {
    auto j = board();
    j.layers.push_back(j.layers.front());
    rejected(j, {"TOP"});
}
void collision() {
    ThicknessOverrides o = {{"top", 35e-6}, {"TOP", 36e-6}};
    rejected(board(), {"top", "TOP"}, {&o, nullptr});
}
void invalid_contours() {
    auto j = board();
    j.profile.contours.push_back(square(2));
    rejected(j, {"multiple profile exteriors"});
    for (bool profile : {true, false}) {
        for (const auto& points :
             std::vector<std::vector<OdbPoint>>{{{1, 1}, {0, 0}, {1, 1}, {0, 0}},
                                                {{1, 1}, {2, 2}, {0, 0}},
                                                {{0.0001, 0}, {0, 0.0001}, {0, 0}},
                                                // Nonzero-area crossing, endpoint contact, and
                                                // adjacent collinear backtracking.
                                                {{4, 4}, {0, 4}, {3, 0}},
                                                {{4, 0}, {4, 4}, {2, 0}, {0, 4}},
                                                {{4, 0}, {2, 0}, {2, 4}, {0, 4}}}) {
            j = board();
            OdbContour c;
            for (const auto& end : points) {
                OdbSegment segment;
                segment.end = end;
                c.segs.push_back(segment);
            }
            if (profile)
                j.profile.contours = {c};
            else
                j.features["TOP"].surfaces.front().contours.push_back(c);
            if (profile) {
                rejected(j, {"degenerate contour"});
                const auto invalid = map_odb(j);
                bool model_unavailable = false;
                try {
                    (void)invalid.value();
                } catch (const std::bad_variant_access&) {
                    model_unavailable = true;
                }
                require(model_unavailable, "invalid profile exposes no model value");
            } else {
                for (bool hole : {false, true})
                    for (bool positive : {false, true}) {
                        auto& surface = j.features["TOP"].surfaces.front();
                        surface.contours.back().is_hole = hole;
                        surface.positive = positive;
                        rejected(j, {"degenerate contour", "TOP"});
                    }
            }
        }
    }
}
void holes_and_profile() {
    auto j = board();
    auto hole = square(2);
    hole.is_hole = true;
    hole.start = {2, 2};
    for (auto& seg : hole.segs) {
        seg.end.x += 2;
        seg.end.y += 2;
    }
    j.features["TOP"].surfaces.front().contours.push_back(hole);
    j.profile.contours.push_back(hole);
    auto m = mapped(j);
    near(area(m), 60e6, 1e-6);
    require(m.report.unsupported.size() == 1 &&
                m.report.unsupported.front().element == "ProfileHole",
            "profile hole report");
    require(m.outline.points.size() == 4, "profile outer contour");
}
// Frozen Clipper2 2.0.1 offset vertices at IPC Y-span resolution.
// Reader-to-reader tests below independently guard round arc and line parity;
// these exact snapshots also guard miter joins and square caps.
void check_arc_stroke(const OdbJob& j, bool rectangular) {
    static const std::vector<std::array<std::int64_t, 2>> references[] = {
        {
            {4039, 1502}, {4101, 1510}, {4162, 1527}, {4221, 1551}, {4276, 1583}, {4326, 1621},
            {4371, 1665}, {4411, 1715}, {4443, 1769}, {4469, 1827}, {4487, 1887}, {4497, 1949},
            {4500, 2013}, {4496, 2065}, {4428, 2583}, {4416, 2645}, {4396, 2705}, {4394, 2710},
            {4194, 3192}, {4166, 3248}, {4131, 3301}, {4129, 3305}, {3811, 3719}, {3769, 3766},
            {3722, 3808}, {3719, 3811}, {3305, 4129}, {3252, 4164}, {3192, 4194}, {2710, 4394},
            {2650, 4414}, {2588, 4427}, {2583, 4428}, {2065, 4496}, {2002, 4500}, {1939, 4496},
            {1877, 4485}, {1817, 4465}, {1759, 4438}, {1706, 4404}, {1657, 4364}, {1614, 4318},
            {1577, 4267}, {1547, 4211}, {1524, 4152}, {1508, 4091}, {1501, 4028}, {1501, 3965},
            {1510, 3902}, {1526, 3841}, {1550, 3783}, {1581, 3728}, {1618, 3677}, {1662, 3631},
            {1711, 3592}, {1765, 3559}, {1823, 3532}, {1883, 3514}, {1935, 3504}, {2387, 3444},
            {2746, 3295}, {3056, 3056}, {3295, 2746}, {3444, 2387}, {3504, 1935}, {3516, 1873},
            {3536, 1813}, {3564, 1756}, {3598, 1703}, {3639, 1654}, {3685, 1612}, {3737, 1575},
            {3792, 1545}, {3851, 1523}, {3913, 1508}, {3976, 1501},
        },
        {
            {2039, -498}, {2101, -490}, {2162, -473}, {2221, -449}, {2276, -417}, {2326, -379},
            {2371, -335}, {2411, -285}, {2443, -231}, {2469, -173}, {2487, -113}, {2497, -51},
            {2500, 13},   {2496, 65},   {2428, 583},  {2416, 645},  {2396, 705},  {2394, 710},
            {2194, 1192}, {2166, 1248}, {2131, 1301}, {2129, 1305}, {1811, 1719}, {1769, 1766},
            {1722, 1808}, {1719, 1811}, {1305, 2129}, {1252, 2164}, {1192, 2194}, {710, 2394},
            {650, 2414},  {588, 2427},  {583, 2428},  {65, 2496},   {2, 2500},    {-61, 2496},
            {-123, 2485}, {-183, 2465}, {-241, 2438}, {-294, 2404}, {-343, 2364}, {-386, 2318},
            {-423, 2267}, {-453, 2211}, {-476, 2152}, {-492, 2091}, {-499, 2028}, {-499, 1965},
            {-490, 1902}, {-474, 1841}, {-450, 1783}, {-419, 1728}, {-382, 1677}, {-338, 1631},
            {-289, 1592}, {-235, 1559}, {-177, 1532}, {-117, 1514}, {-65, 1504},  {387, 1444},
            {746, 1295},  {1056, 1056}, {1295, 746},  {1444, 387},  {1504, -65},  {1516, -127},
            {1536, -187}, {1564, -244}, {1598, -297}, {1639, -346}, {1685, -388}, {1737, -425},
            {1792, -455}, {1851, -477}, {1913, -492}, {1976, -499},
        },
        {
            {-1313, 924},  {-1252, 941},  {-1194, 965},  {-1139, 996},  {-1097, 1028},
            {-834, 1243},  {-569, 1385},  {-296, 1468},  {0, 1497},     {296, 1468},
            {569, 1385},   {834, 1243},   {1097, 1028},  {1148, 991},   {1203, 961},
            {1262, 938},   {1324, 922},   {1386, 915},   {1450, 915},   {1512, 924},
            {1573, 940},   {1632, 964},   {1687, 995},   {1738, 1033},  {1783, 1077},
            {1823, 1126},  {1856, 1180},  {1882, 1237},  {1900, 1298},  {1911, 1360},
            {1914, 1423},  {1909, 1486},  {1896, 1548},  {1875, 1608},  {1847, 1664},
            {1812, 1717},  {1770, 1765},  {1731, 1800},  {1428, 2049},  {1377, 2086},
            {1347, 2104},  {1001, 2289},  {943, 2315},   {910, 2326},   {535, 2440},
            {474, 2455},   {438, 2460},   {48, 2498},    {-15, 2500},   {-48, 2498},
            {-438, 2460},  {-501, 2450},  {-535, 2440},  {-910, 2326},  {-970, 2304},
            {-1001, 2289}, {-1347, 2104}, {-1401, 2071}, {-1428, 2049}, {-1731, 1800},
            {-1778, 1757}, {-1818, 1709}, {-1852, 1655}, {-1879, 1598}, {-1898, 1538},
            {-1910, 1476}, {-1914, 1413}, {-1910, 1350}, {-1898, 1288}, {-1878, 1228},
            {-1851, 1170}, {-1816, 1117}, {-1776, 1069}, {-1729, 1026}, {-1678, 989},
            {-1622, 959},  {-1563, 937},  {-1502, 922},  {-1439, 915},  {-1376, 915},
        },
        {
            {-1902, -490}, {-1841, -474}, {-1783, -450}, {-1728, -419}, {-1677, -382},
            {-1631, -338}, {-1592, -289}, {-1559, -235}, {-1532, -177}, {-1514, -117},
            {-1504, -65},  {-1444, 387},  {-1295, 746},  {-1056, 1056}, {-746, 1295},
            {-387, 1444},  {65, 1504},    {127, 1516},   {187, 1536},   {244, 1564},
            {297, 1598},   {346, 1639},   {388, 1685},   {425, 1737},   {455, 1792},
            {477, 1851},   {492, 1913},   {499, 1976},   {498, 2039},   {490, 2101},
            {473, 2162},   {449, 2221},   {417, 2276},   {379, 2326},   {335, 2371},
            {285, 2411},   {231, 2443},   {173, 2469},   {113, 2487},   {51, 2497},
            {-13, 2500},   {-65, 2496},   {-583, 2428},  {-645, 2416},  {-705, 2396},
            {-710, 2394},  {-1192, 2194}, {-1248, 2166}, {-1301, 2131}, {-1305, 2129},
            {-1719, 1811}, {-1766, 1769}, {-1808, 1722}, {-1811, 1719}, {-2129, 1305},
            {-2164, 1252}, {-2194, 1192}, {-2394, 710},  {-2414, 650},  {-2427, 588},
            {-2428, 583},  {-2496, 65},   {-2500, 2},    {-2496, -61},  {-2485, -123},
            {-2465, -183}, {-2438, -241}, {-2404, -294}, {-2364, -343}, {-2318, -386},
            {-2267, -423}, {-2211, -453}, {-2152, -476}, {-2091, -492}, {-2028, -499},
            {-1965, -499},
        },
        {
            {-1316, -1904}, {-1255, -1888}, {-1197, -1864}, {-1142, -1833}, {-1091, -1796},
            {-1045, -1752}, {-1006, -1703}, {-973, -1649},  {-946, -1591},  {-928, -1531},
            {-917, -1468},  {-914, -1405},  {-919, -1342},  {-932, -1280},  {-953, -1221},
            {-981, -1164},  {-1010, -1120}, {-1330, -679},  {-1475, -232},  {-1475, 232},
            {-1330, 679},   {-1010, 1120},  {-976, 1173},   {-949, 1231},   {-929, 1291},
            {-918, 1353},   {-914, 1416},   {-918, 1479},   {-930, 1541},   {-950, 1601},
            {-978, 1658},   {-1012, 1711},  {-1053, 1759},  {-1099, 1802},  {-1151, 1839},
            {-1206, 1869},  {-1265, 1891},  {-1327, 1906},  {-1390, 1913},  {-1453, 1912},
            {-1515, 1904},  {-1576, 1887},  {-1635, 1863},  {-1690, 1831},  {-1740, 1793},
            {-1785, 1749},  {-1818, 1708},  {-2186, 1202},  {-2220, 1149},  {-2247, 1091},
            {-2258, 1062},  {-2451, 467},   {-2466, 406},   {-2474, 343},   {-2475, 313},
            {-2475, -313},  {-2471, -376},  {-2459, -438},  {-2451, -467},  {-2258, -1062},
            {-2234, -1121}, {-2204, -1176}, {-2186, -1202}, {-1818, -1708}, {-1778, -1757},
            {-1732, -1800}, {-1681, -1837}, {-1625, -1867}, {-1566, -1890}, {-1505, -1906},
            {-1442, -1913}, {-1379, -1913},
        },
        {
            {61, -2496},    {123, -2485},   {183, -2465},   {241, -2438},   {294, -2404},
            {343, -2364},   {386, -2318},   {423, -2267},   {453, -2211},   {476, -2152},
            {492, -2091},   {499, -2028},   {499, -1965},   {490, -1902},   {474, -1841},
            {450, -1783},   {419, -1728},   {382, -1677},   {338, -1631},   {289, -1592},
            {235, -1559},   {177, -1532},   {117, -1514},   {65, -1504},    {-387, -1444},
            {-746, -1295},  {-1056, -1056}, {-1295, -746},  {-1444, -387},  {-1504, 65},
            {-1516, 127},   {-1536, 187},   {-1564, 244},   {-1598, 297},   {-1639, 346},
            {-1685, 388},   {-1737, 425},   {-1792, 455},   {-1851, 477},   {-1913, 492},
            {-1976, 499},   {-2039, 498},   {-2101, 490},   {-2162, 473},   {-2221, 449},
            {-2276, 417},   {-2326, 379},   {-2371, 335},   {-2411, 285},   {-2443, 231},
            {-2469, 173},   {-2487, 113},   {-2497, 51},    {-2500, -13},   {-2496, -65},
            {-2428, -583},  {-2416, -645},  {-2396, -705},  {-2394, -710},  {-2194, -1192},
            {-2166, -1248}, {-2131, -1301}, {-2129, -1305}, {-1811, -1719}, {-1769, -1766},
            {-1722, -1808}, {-1719, -1811}, {-1305, -2129}, {-1252, -2164}, {-1192, -2194},
            {-710, -2394},  {-650, -2414},  {-588, -2427},  {-583, -2428},  {-65, -2496},
            {-2, -2500},
        },
        {
            {48, -2498},    {438, -2460},   {501, -2450},   {535, -2440},   {910, -2326},
            {970, -2304},   {1001, -2289},  {1347, -2104},  {1401, -2071},  {1428, -2049},
            {1731, -1800},  {1778, -1757},  {1818, -1709},  {1852, -1655},  {1879, -1598},
            {1898, -1538},  {1910, -1476},  {1914, -1413},  {1910, -1350},  {1898, -1288},
            {1878, -1228},  {1851, -1170},  {1816, -1117},  {1776, -1069},  {1729, -1026},
            {1678, -989},   {1622, -959},   {1563, -937},   {1502, -922},   {1439, -915},
            {1376, -915},   {1313, -924},   {1252, -941},   {1194, -965},   {1139, -996},
            {1097, -1028},  {834, -1243},   {569, -1385},   {296, -1468},   {0, -1497},
            {-296, -1468},  {-569, -1385},  {-834, -1243},  {-1097, -1028}, {-1148, -991},
            {-1203, -961},  {-1262, -938},  {-1324, -922},  {-1386, -915},  {-1450, -915},
            {-1512, -924},  {-1573, -940},  {-1632, -964},  {-1687, -995},  {-1738, -1033},
            {-1783, -1077}, {-1823, -1126}, {-1856, -1180}, {-1882, -1237}, {-1900, -1298},
            {-1911, -1360}, {-1914, -1423}, {-1909, -1486}, {-1896, -1548}, {-1875, -1608},
            {-1847, -1664}, {-1812, -1717}, {-1770, -1765}, {-1731, -1800}, {-1428, -2049},
            {-1377, -2086}, {-1347, -2104}, {-1001, -2289}, {-943, -2315},  {-910, -2326},
            {-535, -2440},  {-474, -2455},  {-438, -2460},  {-48, -2498},   {15, -2500},
        },
        {
            {65, -2496},   {583, -2428},  {645, -2416},  {705, -2396},  {710, -2394},
            {1192, -2194}, {1248, -2166}, {1301, -2131}, {1305, -2129}, {1719, -1811},
            {1766, -1769}, {1808, -1722}, {1811, -1719}, {2129, -1305}, {2164, -1252},
            {2194, -1192}, {2394, -710},  {2414, -650},  {2427, -588},  {2428, -583},
            {2496, -65},   {2500, -2},    {2496, 61},    {2485, 123},   {2465, 183},
            {2438, 241},   {2404, 294},   {2364, 343},   {2318, 386},   {2267, 423},
            {2211, 453},   {2152, 476},   {2091, 492},   {2028, 499},   {1965, 499},
            {1902, 490},   {1841, 474},   {1783, 450},   {1728, 419},   {1677, 382},
            {1631, 338},   {1592, 289},   {1559, 235},   {1532, 177},   {1514, 117},
            {1504, 65},    {1444, -387},  {1295, -746},  {1056, -1056}, {746, -1295},
            {387, -1444},  {-65, -1504},  {-127, -1516}, {-187, -1536}, {-244, -1564},
            {-297, -1598}, {-346, -1639}, {-388, -1685}, {-425, -1737}, {-455, -1792},
            {-477, -1851}, {-492, -1913}, {-499, -1976}, {-498, -2039}, {-490, -2101},
            {-473, -2162}, {-449, -2221}, {-417, -2276}, {-379, -2326}, {-335, -2371},
            {-285, -2411}, {-231, -2443}, {-173, -2469}, {-113, -2487}, {-51, -2497},
            {13, -2500},
        },
        {
            {1453, -1912}, {1515, -1904}, {1576, -1887}, {1635, -1863}, {1690, -1831},
            {1740, -1793}, {1785, -1749}, {1818, -1708}, {2186, -1202}, {2220, -1149},
            {2247, -1091}, {2258, -1062}, {2451, -467},  {2466, -406},  {2474, -343},
            {2475, -313},  {2475, 313},   {2471, 376},   {2459, 438},   {2451, 467},
            {2258, 1062},  {2234, 1121},  {2204, 1176},  {2186, 1202},  {1818, 1708},
            {1778, 1757},  {1732, 1800},  {1681, 1837},  {1625, 1867},  {1566, 1890},
            {1505, 1906},  {1442, 1913},  {1379, 1913},  {1316, 1904},  {1255, 1888},
            {1197, 1864},  {1142, 1833},  {1091, 1796},  {1045, 1752},  {1006, 1703},
            {973, 1649},   {946, 1591},   {928, 1531},   {917, 1468},   {914, 1405},
            {919, 1342},   {932, 1280},   {953, 1221},   {981, 1164},   {1010, 1120},
            {1330, 679},   {1475, 232},   {1475, -232},  {1330, -679},  {1010, -1120},
            {976, -1173},  {949, -1231},  {929, -1291},  {918, -1353},  {914, -1416},
            {918, -1479},  {930, -1541},  {950, -1601},  {978, -1658},  {1012, -1711},
            {1053, -1759}, {1099, -1802}, {1151, -1839}, {1206, -1869}, {1265, -1891},
            {1327, -1906}, {1390, -1913},
        },
        {
            {2561, -431},
            {2419, 649},
            {2169, 1252},
            {1771, 1771},
            {1252, 2169},
            {649, 2419},
            {-431, 2561},
            {-561, 1569},
            {387, 1444},
            {746, 1295},
            {1056, 1056},
            {1295, 746},
            {1444, 387},
            {1569, -561},
        },
        {
            {2118, 1483},
            {1390, 2081},
            {957, 2312},
            {488, 2455},
            {0, 2502},
            {-488, 2455},
            {-957, 2312},
            {-1390, 2081},
            {-2118, 1483},
            {-1483, 710},
            {-834, 1243},
            {-569, 1385},
            {-296, 1468},
            {0, 1497},
            {296, 1468},
            {569, 1385},
            {834, 1243},
            {1483, 710},
        },
        {
            {-1444, 387},
            {-1295, 746},
            {-1056, 1056},
            {-746, 1295},
            {-387, 1444},
            {561, 1569},
            {431, 2561},
            {-649, 2419},
            {-1252, 2169},
            {-1771, 1771},
            {-2169, 1252},
            {-2419, 649},
            {-2561, -431},
            {-1569, -561},
        },
        {
            {-716, -1524},
            {-1330, -679},
            {-1475, -232},
            {-1475, 232},
            {-1330, 679},
            {-716, 1524},
            {-1524, 2112},
            {-2233, 1138},
            {-2475, 392},
            {-2475, -392},
            {-2233, -1138},
            {-1524, -2112},
        },
        {
            {561, -1569},
            {-387, -1444},
            {-746, -1295},
            {-1056, -1056},
            {-1295, -746},
            {-1444, -387},
            {-1569, 561},
            {-2561, 431},
            {-2419, -649},
            {-2169, -1252},
            {-1771, -1771},
            {-1252, -2169},
            {-649, -2419},
            {431, -2561},
        },
        {
            {488, -2455},
            {957, -2312},
            {1390, -2081},
            {2118, -1483},
            {1483, -710},
            {834, -1243},
            {569, -1385},
            {296, -1468},
            {0, -1497},
            {-296, -1468},
            {-569, -1385},
            {-834, -1243},
            {-1483, -710},
            {-2118, -1483},
            {-1390, -2081},
            {-957, -2312},
            {-488, -2455},
            {0, -2502},
        },
        {
            {649, -2419},
            {1252, -2169},
            {1771, -1771},
            {2169, -1252},
            {2419, -649},
            {2561, 431},
            {1569, 561},
            {1444, -387},
            {1295, -746},
            {1056, -1056},
            {746, -1295},
            {387, -1444},
            {-561, -1569},
            {-431, -2561},
        },
        {
            {2233, -1138},
            {2475, -392},
            {2475, 392},
            {2233, 1138},
            {1524, 2112},
            {716, 1524},
            {1330, 679},
            {1475, 232},
            {1475, -232},
            {1330, -679},
            {716, -1524},
            {1524, -2112},
        },
        {
            {4561, 1569},
            {4419, 2649},
            {4169, 3252},
            {3771, 3771},
            {3252, 4169},
            {2649, 4419},
            {1569, 4561},
            {1439, 3569},
            {2387, 3444},
            {2746, 3295},
            {3056, 3056},
            {3295, 2746},
            {3444, 2387},
            {3569, 1439},
        },
    };
    const auto& arc = j.features.at("TOP").arcs.front();
    std::size_t index = rectangular ? 17 : 0;
    if (arc.c.x == 0 && arc.c.y == 0) {
        const double angle = std::atan2(arc.s.y, arc.s.x);
        const int eighth = (static_cast<int>(std::llround(angle * 4 / std::numbers::pi)) + 8) % 8;
        index = 1 + (rectangular ? 8 : 0) + eighth;
    }
    const auto& expected = references[index];
    const auto m = mapped(j);
    require(m.layers.front().polygons.size() == 1, "one arc stroke contour");
    const auto& actual = m.layers.front().polygons.front().points;
    require(actual.size() == expected.size() &&
                std::set<std::array<std::int64_t, 2>>(actual.begin(), actual.end()) ==
                    std::set<std::array<std::int64_t, 2>>(expected.begin(), expected.end()),
            "IPC-resolution arc stroke vertices and joins");
}
void rect_strokes_and_rotation() {
    for (bool rectangular : {false, true}) {
        auto j = board();
        auto& f = j.features["TOP"];
        f.surfaces.clear();
        f.symbols = {{rectangular ? "rect1000x1000" : "round1000"}};
        if (rectangular) {
            f.lines = {{{1, 1}, {7, 1}, 0, true}};
            const auto m = mapped(j);
            near(area(m), 7e6);
            require(m.layers.front().polygons.front().points.size() == 4,
                    "rect line has four square corners");
            f.lines.clear();
        }
        for (int eighth = 0; eighth < 8; ++eighth) {
            const double angle = eighth * std::numbers::pi / 4;
            OdbPoint start{2 * std::cos(angle), 2 * std::sin(angle)};
            OdbPoint end{-2 * std::sin(angle), 2 * std::cos(angle)};
            f.arcs = {{start, end, {0, 0}, 0, true, false}};
            check_arc_stroke(j, rectangular);
        }
    }
}
void pads_and_arcs() {
    auto j = board();
    auto& f = j.features["TOP"];
    f.surfaces.clear();
    f.symbols = {{"ReCt2000x1000"}, {"OvAl2000x1000"}, {"ROUND1000"}};
    f.pads = {{{2, 2}, 0, true, 90, true}};
    auto m = mapped(j);
    near(area(m), 2e6);
    std::int64_t xmin = 100000, xmax = -100000, ymin = 100000, ymax = -100000;
    for (const auto& polygon : m.layers[0].polygons)
        for (const auto& p : polygon.points) {
            xmin = std::min(xmin, p[0]);
            xmax = std::max(xmax, p[0]);
            ymin = std::min(ymin, p[1]);
            ymax = std::max(ymax, p[1]);
        }
    require(xmin == 1500 && xmax == 2500 && ymin == 1000 && ymax == 3000,
            "rotated mirrored rectangle");
    f.pads = {{{2, 2}, 1, true}};
    near(area(mapped(j)), 1e6 + std::numbers::pi * 250000, 1e-3);
    f.pads = {{{2, 2}, 2, true}};
    near(area(mapped(j)), std::numbers::pi * 250000, 1e-3);
    f.pads.clear();
    f.arcs = {{{4, 2}, {2, 4}, {2, 2}, 2, true, false}};
    check_arc_stroke(j, false);
    auto inch = j;
    inch.units = OdbUnits::inch;
    for (auto& [name, attrs] : inch.attrs.layer)
        attrs[".layer_dielectric"] = name == "TOP" || name == "BOTTOM" ? "0.0013779527559055118"
                                     : name == "CORE"                  ? "0.031496062992125984"
                                                                       : "0.007874015748031496";
    for (auto& c : inch.profile.contours) {
        c.start.x /= 25.4;
        c.start.y /= 25.4;
        for (auto& seg : c.segs) {
            seg.end.x /= 25.4;
            seg.end.y /= 25.4;
        }
    }
    auto& arc = inch.features["TOP"].arcs.front();
    for (auto* p : {&arc.s, &arc.e, &arc.c}) {
        p->x /= 25.4;
        p->y /= 25.4;
    }
    inch.features["TOP"].symbols[2].name = "round39.37007874015748";
    equal(mapped(j), mapped(inch));
}
// Exercise the real IPC reader at both orientations, including its Y-span tolerance.
void ipc_arc_equivalence() {
    std::random_device random;
    const auto dir =
        std::filesystem::temp_directory_path() / ("odb-ipc-arc-" + std::to_string(random()));
    require(std::filesystem::create_directory(dir), "private IPC fixture directory");
    struct Cleanup {
        std::filesystem::path dir;
        ~Cleanup() { std::filesystem::remove_all(dir); }
    } cleanup{dir};
    for (int orientation : {0, 1, 2, 3, 4, 5, 6, 7, 8, 9}) {
        const bool rotated = orientation % 2;
        const bool semicircle = orientation >= 2;
        auto j = board();
        auto& f = j.features["TOP"];
        f.surfaces.clear();
        f.symbols = {{"round1000"}};
        const double radius = orientation >= 4 ? 2 : semicircle ? 0.5 : 2;
        const OdbPoint start = rotated ? OdbPoint{2, 2 + radius} : OdbPoint{2 + radius, 2};
        const OdbPoint end = semicircle
                                 ? (rotated ? OdbPoint{2, 2 - radius} : OdbPoint{2 - radius, 2})
                                 : (rotated ? OdbPoint{2 - radius, 2} : OdbPoint{2, 2 + radius});
        const bool line = orientation >= 8;
        const OdbPoint finish = orientation == 9 ? start : end;
        if (line)
            f.lines = {{start, finish, 0, true}};
        else
            f.arcs = {{start, finish, {2, 2}, 0, true, false}};
        std::ostringstream xml;
        xml << R"(<IPC-2581><Ecad><CadData><Layer name="TOP" layerFunction="CONDUCTOR"/>
<Stackup><StackupGroup name="S"><StackupLayer layerOrGroupRef="TOP" thickness="0.035"/>
</StackupGroup></Stackup><Step name="board"><Profile><Polygon><PolyBegin x="0" y="0"/>
<PolyStepSegment x="10" y="0"/><PolyStepSegment x="10" y="10"/>
<PolyStepSegment x="0" y="10"/>
</Polygon></Profile><LayerFeature layerRef="TOP"><Set><)"
            << (line ? "Line" : "Arc") << R"( startX=")" << start.x << "\" startY=\"" << start.y
            << "\" endX=\"" << finish.x << "\" endY=\"" << finish.y
            << R"(" centerX="2" centerY="2" clockwise="false"><LineDesc lineWidth="1" lineEnd="ROUND"/>
</)" << (line ? "Line" : "Arc")
            << R"(></Set></LayerFeature></Step></CadData></Ecad></IPC-2581>)";
        const auto file = dir / "arc.xml";
        {
            std::ofstream out(file);
            out << xml.str();
        }
        const auto ipc = read_ipc2581(file);
        require(bool(ipc), "IPC reference imports: " + ipc.error);
        const auto odb = mapped(j);
        require(ipc.model.layers.size() == 1 && ipc.model.report.unsupported.empty(),
                "IPC reference has one supported copper layer");
        require(odb.layers.front().polygons.size() == 1 &&
                    ipc.model.layers.front().polygons.size() == 1,
                "one stroke from each reader");
        require(odb.outline.points == ipc.model.outline.points && !odb.outline.points.empty(),
                "ODB and IPC fixture outlines agree exactly");
        near(area(odb), area(ipc.model), 1e-12);
        const auto& a = odb.layers.front().polygons.front().points;
        const auto& b = ipc.model.layers.front().polygons.front().points;
        require(a.size() == b.size() &&
                    std::set<std::array<std::int64_t, 2>>(a.begin(), a.end()) ==
                        std::set<std::array<std::int64_t, 2>>(b.begin(), b.end()),
                "ODB and IPC arc/line vertices agree exactly");
    }
}
void rectangular_traces() {
    auto j = board();
    auto& f = j.features["TOP"];
    f.surfaces.clear();
    f.symbols = {{"rect1000x1000"}};
    f.lines = {{{1, 1}, {7, 1}, 0, true}};
    near(area(mapped(j)), 7e6, 1e-3);
    f.lines.clear();
    f.arcs = {{{4, 2}, {2, 4}, {2, 2}, 0, true, false}};
    check_arc_stroke(j, true);
}
void boundaries() {
    rejected(OdbJob{}, {"empty"});
    auto j = board();
    std::reverse(j.layers.begin(), j.layers.end());
    for (auto& l : j.layers) {
        if (l.type == "signal")
            l.type = "SIGNAL";
        if (l.type == "dielectric")
            l.type = "DIELECTRIC";
    }
    equal(mapped(board()), mapped(j));
    ThicknessOverrides o = {{"top", 70e-6}};
    std::vector<ThicknessRecord> records;
    const auto m = mapped(j, {&o, &records});
    const double odb = mapped(board()).layers.front().thickness_m;
    near(m.layers.front().thickness_m, odb, 1e-12);
    near(records.front().thickness_m, odb, 1e-12);
    require(records.front().source == "odb" && records.front().odb_value_m,
            "positive ODB wins over case-insensitive override");
    near(*records.front().odb_value_m, odb, 1e-12);
    o["top"] = -1;
    near(mapped(j, {&o, nullptr}).layers.front().thickness_m, odb, 1e-12);
    j.attrs.layer["TOP"][".layer_dielectric"] = "0";
    rejected(j, {"top"}, {&o, nullptr});
}
void degenerate_copper() {
    auto j = board();
    auto& f = j.features["TOP"];
    f.surfaces.clear();
    f.surfaces.push_back({true, {square(0), square(0.0001), square(8)}});
    OdbContour line;
    line.segs = {{{4, 0}, false, {}, false}, {{8, 0}, false, {}, false}};
    f.surfaces.push_back({true, {line}});
    f.surfaces.push_back({false, {square(4)}});
    rejected(j, {"degenerate contour", "TOP"});
    f.surfaces = {{true, {square(8)}}, {false, {square(4)}}};
    auto m = mapped(j);
    near(area(m), 48e6);
    for (const auto& polygon : m.layers.front().polygons) {
        require(polygon.points.size() >= 3, "copper needs three vertices");
        long double twice_area = 0;
        for (std::size_t i = 0; i < polygon.points.size(); ++i) {
            const auto& a = polygon.points[i];
            const auto& b = polygon.points[(i + 1) % polygon.points.size()];
            require(a != b, "no duplicate copper edge vertices");
            twice_area +=
                static_cast<long double>(a[0]) * b[1] - static_cast<long double>(b[0]) * a[1];
        }
        require(twice_area != 0, "copper needs nonzero area");
    }
}
void boolean_ring_invariants() {
    auto j = board();
    auto& f = j.features["TOP"];
    // A sub-DBU gap closes on rounding, joining the hole to the exterior
    // at one point. The boolean result must never emit a self-touching ring.
    OdbContour diamond;
    diamond.start = {0.0004, 4};
    diamond.segs = {{{2, 2}, false, {}, false},
                    {{4, 4}, false, {}, false},
                    {{2, 6}, false, {}, false},
                    {diamond.start, false, {}, false}};
    f.surfaces.push_back({false, {diamond}});
    auto result = map_odb(j);
    require(bool(result), "point contact is represented by separate simple rings");
    require(result.value().layers.front().polygons.size() == 2,
            "rounded point contact preserves exterior and hole");
    near(area(result.value()), 56e6);

    // Four individually simple triangles produce a non-simple union when the
    // narrow intersections round to integer DBU. Reject instead of exporting it.
    auto tiny = board();
    auto& surfaces = tiny.features["TOP"].surfaces;
    surfaces.clear();
    const std::array<std::array<OdbPoint, 3>, 4> triangles = {{
        {{{0.011, 0.019}, {0.007, 0.006}, {0.017, 0.012}}},
        {{{0.001, 0.014}, {0.016, 0.018}, {0.014, 0.016}}},
        {{{0.016, 0.004}, {0.010, 0.006}, {0.017, 0.012}}},
        {{{0, 0.012}, {0.017, 0.008}, {0.008, 0.019}}},
    }};
    for (const auto& vertices : triangles) {
        OdbContour triangle;
        triangle.start = vertices[0];
        for (const auto& point : {vertices[1], vertices[2], vertices[0]}) {
            OdbSegment seg;
            seg.end = point;
            triangle.segs.push_back(seg);
        }
        surfaces.push_back({true, {triangle}});
    }
    rejected(tiny, {"invalid copper output ring", "TOP"});
    // A genuine hole and an island exercise alternating winding by depth.
    f.surfaces.pop_back();
    auto hole = square(4);
    hole.start = {1, 1};
    for (auto& seg : hole.segs) {
        seg.end.x += 1;
        seg.end.y += 1;
    }
    hole.is_hole = true;
    f.surfaces.front().contours.push_back(hole);
    auto island = square(1);
    island.start = {2, 2};
    for (auto& seg : island.segs) {
        seg.end.x += 2;
        seg.end.y += 2;
    }
    f.surfaces.push_back({true, {island}});
    const auto m = mapped(j);
    require(m.layers.front().polygons.size() == 3, "exterior, hole and island retained");
    int positive = 0, negative = 0;
    for (const auto& polygon : m.layers.front().polygons) {
        long double sum = 0;
        std::set<std::array<std::int64_t, 2>> vertices;
        for (std::size_t i = 0; i < polygon.points.size(); ++i) {
            const auto& a = polygon.points[i];
            const auto& b = polygon.points[(i + 1) % polygon.points.size()];
            require(vertices.insert(a).second, "boolean ring has no repeated vertices");
            sum += static_cast<long double>(a[0]) * b[1] - static_cast<long double>(b[0]) * a[1];
        }
        require(sum != 0, "boolean ring has nonzero area");
        sum > 0 ? ++positive : ++negative;
    }
    require(positive == 2 && negative == 1, "exteriors CCW and holes CW");
    near(area(m), 49e6);
}
void unknown_drill_tools() {
    auto j = board();
    OdbLayer l;
    l.name = "DRILL";
    l.type = "drill";
    l.start_name = "TOP";
    l.end_name = "BOTTOM";
    j.layers.push_back(l);
    auto& f = j.features["DRILL"];
    f.symbols = {{"round600"}};
    f.pads = {{{2, 3}, 0, true}};
    j.attrs.layer["DRILL"][".plating_thickness"] = "0.025";
    for (const auto* type : {"", "unknown", "laser", "platted"}) {
        j.tools["DRILL"] = {{1, type, 0.6}};
        auto m = mapped(j);
        require(m.vias.empty() && m.via_fractions.empty(), "unknown tools cannot create vias");
        require(m.report.unsupported.size() == 1 &&
                    m.report.unsupported.front().element ==
                        std::string("ViaToolUnsupported:DRILL:") + type,
                "unknown tool classification reported");
        j.tools["DRILL"].push_back({2, "plated", 0.6});
        for (int order = 0; order < 2; ++order) {
            m = mapped(j);
            require(m.vias.empty() && m.report.unsupported.size() == 1 &&
                        m.report.unsupported.front().element == "ViaToolAmbiguous:DRILL",
                    "unknown and plated tool ambiguity is order independent");
            std::reverse(j.tools["DRILL"].begin(), j.tools["DRILL"].end());
        }
    }
}
void via_radii() {
    auto j = board();
    OdbLayer l;
    l.name = "DRILL";
    l.type = "drill";
    l.start_name = "TOP";
    l.end_name = "BOTTOM";
    j.layers.push_back(l);
    auto& f = j.features["DRILL"];
    f.symbols = {{"round600"}};
    f.pads = {{{2, 3}, 0, true}};
    for (bool with_tool : {false, true}) {
        if (with_tool)
            j.tools["DRILL"] = {{1, "plated", 0.6}};
        for (const auto* plating : {"0.025", "0.3", "0.0001"}) {
            j.attrs.layer["DRILL"][".plating_thickness"] = plating;
            auto m = mapped(j);
            require(m.vias.size() == 1, "valid annulus retained");
            const auto& v = m.vias.front();
            require(0 <= v.inner_radius_dbu && v.inner_radius_dbu < v.outer_radius_dbu,
                    "valid via radius invariant");
        }
        j.attrs.layer["DRILL"][".plating_thickness"] = "0.0001";
        near(mapped(j).vias.front().inner_radius_dbu, 299.9, 1e-12);
        for (const auto* plating : {"0.301", "1e-300"}) {
            j.attrs.layer["DRILL"][".plating_thickness"] = plating;
            auto m = mapped(j);
            require(m.vias.empty() && m.via_fractions.empty(), "invalid annulus omitted");
            require(m.report.unsupported.size() == 1 &&
                        m.report.unsupported.front().element == "ViaGeometry:DRILL",
                    "invalid annulus reported");
        }
    }
    f.symbols = {{"round0.5"}};
    j.attrs.layer["DRILL"][".plating_thickness"] = "0.0001";
    for (double diameter : {0.0, -0.0001}) {
        j.tools["DRILL"] = {{1, "plated", diameter}};
        auto m = mapped(j);
        require(m.vias.size() == 1 && m.report.unsupported.empty(),
                "unmatched nonpositive tool does not replace the positive symbol diameter");
        near(m.vias.front().outer_radius_dbu, 0.5);
        near(m.vias.front().inner_radius_dbu, 0.4);
    }
}
void drill_rejections() {
    auto j = board();
    OdbLayer l;
    l.name = "DRILL";
    l.type = "DRILL";
    l.start_name = "TOP";
    l.end_name = "BOTTOM";
    j.layers.push_back(l);
    j.features["DRILL"].symbols = {{"round600"}};
    j.features["DRILL"].pads = {{{2, 3}, 0, true}};
    auto m = mapped(j);
    require(m.vias.empty() && m.report.unsupported.size() == 1 &&
                m.report.unsupported.front().element == "ViaPlatingMissing:DRILL",
            "no default plating");
    for (const auto* text : {"invalid", "0.025junk", "nan", ""}) {
        j.attrs.layer["DRILL"][".plating_thickness"] = text;
        m = mapped(j);
        require(m.vias.empty() && m.report.unsupported.size() == 1 &&
                    m.report.unsupported.front().element == "ViaPlatingMissing:DRILL",
                "malformed plating reported");
    }
    // The frozen OdbAttrs contract stores layers/<name>/attrlist by layer name.
    j.attrs.job[".plating_thickness"] = "0.2";
    j.attrs.layer["TOP"][".plating_thickness"] = "0.1";
    j.attrs.layer["DRILL"][".plating_thickness"] = "0.025";
    m = mapped(j);
    require(m.vias.size() == 1 && m.report.unsupported.empty(),
            "drill layer attrlist supplies plating independently of job/copper attrs");
    near(m.vias.front().inner_radius_dbu, 275);
    j.attrs.layer["DRILL"].erase(".plating_thickness");
    m = mapped(j);
    require(m.vias.empty() && m.report.unsupported.size() == 1 &&
                m.report.unsupported.front().element == "ViaPlatingMissing:DRILL",
            "unrelated attrlists cannot supply drill plating");
    j.attrs.layer["DRILL"][".plating_thickness"] = "0.025";
    j.layers.back().end_name.clear();
    m = mapped(j);
    require(m.vias.empty() && m.report.unsupported.size() == 1 &&
                m.report.unsupported.front().element == "ViaSpan:DRILL",
            "no default span");
}

OdbContour polygon_contour(const std::vector<OdbPoint>& vertices, bool hole = false) {
    require(vertices.size() >= 3, "synthetic contour needs three vertices");
    OdbContour c;
    c.is_hole = hole;
    c.start = vertices.front();
    for (std::size_t i = 1; i < vertices.size(); ++i)
        c.segs.push_back({vertices[i], false, {}, false});
    c.segs.push_back({c.start, false, {}, false});
    return c;
}
OdbContour shifted(OdbContour c, double x, double y) {
    c.start.x += x;
    c.start.y += y;
    for (auto& s : c.segs) {
        s.end.x += x;
        s.end.y += y;
        s.center.x += x;
        s.center.y += y;
    }
    return c;
}
OdbContour hole_square(double x, double y) {
    auto c = shifted(square(2), x, y);
    c.is_hole = true;
    return c;
}
OdbContour two_bridged_holes() {
    // CCW exterior, CW holes; bridge feet split the lower exterior edge.
    return polygon_contour({{0, 0},
                            {1, 0},
                            {1, 1},
                            {1, 3},
                            {3, 3},
                            {3, 1},
                            {1, 1},
                            {1, 0},
                            {5, 0},
                            {5, 1},
                            {5, 3},
                            {7, 3},
                            {7, 1},
                            {5, 1},
                            {5, 0},
                            {8, 0},
                            {8, 8},
                            {0, 8}});
}
void same_polygon_rings(const EcadModel& actual,
                        const EcadModel& expected,
                        std::size_t positive,
                        std::size_t negative) {
    using Ring = std::vector<std::array<std::int64_t, 2>>;
    auto canonical = [&](const EcadModel& model) {
        std::vector<Ring> rings;
        std::size_t ccw = 0, cw = 0;
        for (const auto& polygon : model.layers.front().polygons) {
            std::int64_t twice_area = 0;
            const auto& points = polygon.points;
            for (std::size_t i = 0; i < points.size(); ++i) {
                const auto& a = points[i];
                const auto& b = points[(i + 1) % points.size()];
                twice_area += a[0] * b[1] - b[0] * a[1];
            }
            require(twice_area != 0, "output ring has nonzero signed area");
            twice_area > 0 ? ++ccw : ++cw;
            auto ring = points;
            std::sort(ring.begin(), ring.end());
            rings.push_back(std::move(ring));
        }
        require(ccw == positive && cw == negative, "exteriors CCW and holes CW");
        std::sort(rings.begin(), rings.end());
        return rings;
    };
    require(actual.layers.front().polygons.size() == positive + negative &&
                actual.layers.front().polygons.size() == expected.layers.front().polygons.size(),
            "equivalent polygon count");
    require(canonical(actual) == canonical(expected), "equivalent integer polygon multisets");
}
void equivalent_surface(const OdbContour& contour,
                        const std::vector<OdbContour>& independent,
                        double expected_area,
                        std::size_t positive,
                        std::size_t negative) {
    auto reference = board();
    reference.features["TOP"].surfaces = {{true, independent}};
    const auto expected = mapped(reference);
    near(area(expected), expected_area, 0);
    auto j = board();
    j.features["TOP"].surfaces = {{true, {contour}}};
    const auto actual = mapped(j);
    near(area(actual), expected_area, 0);
    near(area(actual), area(expected), 0);
    same_polygon_rings(actual, expected, positive, negative);
}
void bridge_subcases(std::initializer_list<std::pair<const char*, std::function<void()>>> cases) {
    std::string failures;
    for (const auto& [name, test] : cases) {
        try {
            test();
            std::cout << "PASS bridge subcase " << name << '\n';
        } catch (const std::exception& e) {
            const auto message = std::string(name) + ": " + e.what();
            std::cerr << "FAIL bridge subcase " << message << '\n';
            failures += message + "; ";
        }
    }
    require(failures.empty(), failures);
}
OdbContour repeated_bridge() {
    // Four CCW triangular islands, two at each endpoint of the same bridge.
    // Each bridge direction occurs twice, with a boundary loop between visits.
    return polygon_contour({{4, 4},
                            {4, 6},
                            {6, 6},
                            {6, 8},
                            {4, 6},
                            {4, 4},
                            {2, 4},
                            {2, 2},
                            {4, 4},
                            {4, 6},
                            {2, 8},
                            {2, 6},
                            {4, 6},
                            {4, 4},
                            {6, 2},
                            {6, 4}});
}
void bridged_surface() {
    bridge_subcases({
        {"two holes",
         [] {
             equivalent_surface(two_bridged_holes(),
                                {square(8), hole_square(1, 1), hole_square(5, 1)},
                                56e6,
                                1,
                                2);
         }},
        {"wide exact area and reversed winding",
         [] {
             auto reference = board();
             reference.features["TOP"].surfaces = {
                 {true, {square(8), hole_square(1, 1), hole_square(5, 1)}}};
             const auto expected = mapped(reference);
             // 2^32 dbu makes some coordinate products exactly 2^64.
             // Both signs and windings exercise exact cancellation beyond int64.
             for (double x : {-4294967.296, 4294967.296})
                 for (double y : {-4294967.296, 4294967.296})
                     for (bool reverse : {false, true}) {
                         auto c = two_bridged_holes();
                         if (reverse) {
                             std::vector<OdbPoint> vertices{c.start};
                             for (std::size_t i = 0; i + 1 < c.segs.size(); ++i)
                                 vertices.push_back(c.segs[i].end);
                             std::reverse(vertices.begin(), vertices.end());
                             c = polygon_contour(vertices);
                         }
                         auto j = board();
                         j.profile.contours = {shifted(square(10), x, y)};
                         j.features["TOP"].surfaces = {{true, {shifted(c, x, y)}}};
                         auto actual = mapped(j);
                         // Compare in local coordinates so test-only area helpers
                         // need neither overflowing int64 products nor cancellation
                         // of rounded double products at the translated origin.
                         for (auto& polygon : actual.layers.front().polygons)
                             for (auto& point : polygon.points) {
                                 point[0] -= std::llround(x * 1000);
                                 point[1] -= std::llround(y * 1000);
                             }
                         near(area(actual), 56e6, 0);
                         same_polygon_rings(actual, expected, 1, 2);
                     }
         }},
        {"repeated bridge pairs",
         [] {
             equivalent_surface(repeated_bridge(),
                                {polygon_contour({{4, 4}, {2, 4}, {2, 2}}),
                                 polygon_contour({{4, 4}, {6, 2}, {6, 4}}),
                                 polygon_contour({{4, 6}, {6, 6}, {6, 8}}),
                                 polygon_contour({{4, 6}, {2, 8}, {2, 6}})},
                                8e6,
                                4,
                                0);
         }},
        {"same-winding touching islands without bridges",
         [] {
             equivalent_surface(
                 polygon_contour({{0, 0}, {4, 0}, {4, 4}, {5, 4}, {5, 5}, {4, 5}, {4, 4}, {0, 4}}),
                 {square(4), shifted(square(1), 4, 4)},
                 17e6,
                 2,
                 0);
         }},
        {"negative surface",
         [] {
             auto reference = board();
             reference.features["TOP"].surfaces = {
                 {true, {square(10)}},
                 {false, {shifted(square(8), 1, 1), hole_square(2, 2), hole_square(6, 2)}}};
             const auto expected = mapped(reference);
             near(area(expected), 44e6, 0);
             auto j = board();
             j.features["TOP"].surfaces = {{true, {square(10)}},
                                           {false, {shifted(two_bridged_holes(), 1, 1)}}};
             const auto actual = mapped(j);
             near(area(actual), 44e6, 0);
             near(area(actual), area(expected), 0);
             same_polygon_rings(actual, expected, 3, 1);
         }},
        {"arc-discretized bridge",
         [] {
             auto c = polygon_contour({{0, 0},
                                       {1, 0},
                                       {2, 1},
                                       {2, 3},
                                       {4, 3},
                                       {4, 1},
                                       {2, 1},
                                       {1, 0},
                                       {8, 0},
                                       {8, 8},
                                       {0, 8}});
             c.segs[1] = {{2, 1}, true, {1, 1}, false};
             c.segs[6] = {{1, 0}, true, {1, 1}, true};
             equivalent_surface(c, {square(8), hole_square(2, 1)}, 60e6, 1, 1);
         }},
    });
}
void bridged_hole_chain() {
    const auto c = polygon_contour({{0, 0},
                                    {1, 0},
                                    {1, 1},
                                    {1, 3},
                                    {1, 5},
                                    {1, 7},
                                    {3, 7},
                                    {3, 5},
                                    {1, 5},
                                    {1, 3},
                                    {3, 3},
                                    {3, 1},
                                    {1, 1},
                                    {1, 0},
                                    {8, 0},
                                    {8, 8},
                                    {0, 8}});
    equivalent_surface(c, {square(8), hole_square(1, 1), hole_square(1, 5)}, 56e6, 1, 2);
}
void bridge_rejections() {
    auto reject_surface = [](const OdbContour& c) {
        auto j = board();
        j.features["TOP"].surfaces = {{true, {c}}};
        rejected(j, {"degenerate contour", "TOP"});
    };
    bridge_subcases({
        {"T contact",
         [&] {
             auto c = two_bridged_holes();
             c.segs[2].end = {1, 8}; // Hole vertex inside the unsplit exterior upper edge.
             reject_surface(c);
         }},
        {"bridge crossing a hole",
         [&] {
             reject_surface(polygon_contour({{0, 0}, {2, 0}, {2, 4}, {1, 1}, {1, 3}, {3, 3}, {3, 1},
                                             {1, 1}, {2, 4}, {2, 0}, {5, 0}, {5, 1}, {5, 3}, {7, 3},
                                             {7, 1}, {5, 1}, {5, 0}, {8, 0}, {8, 8}, {0, 8}}));
         }},
        {"distinct bridge pairs crossing",
         [&] {
             // Each diagonal is traversed in both directions and touches only
             // its own hole. They cross at an interior point of both bridges.
             std::vector<OdbPoint> points{{0, 0},
                                          {2, 0},
                                          {7, 6},
                                          {7, 7},
                                          {8, 7},
                                          {8, 6},
                                          {7, 6},
                                          {2, 0},
                                          {8, 0},
                                          {3, 6},
                                          {2, 6},
                                          {2, 7},
                                          {3, 7},
                                          {3, 6},
                                          {8, 0},
                                          {10, 0},
                                          {10, 10},
                                          {0, 10}};
             reject_surface(polygon_contour(points));
             std::reverse(points.begin(), points.end());
             reject_surface(polygon_contour(points));
         }},
        {"opposite-winding exterior figure-eight",
         [&] {
             reject_surface(
                 polygon_contour({{0, 0}, {8, 0}, {8, 8}, {8, 9}, {9, 9}, {9, 8}, {8, 8}, {0, 8}}));
         }},
        {"unmatched bridge multiplicity",
         [&] {
             auto c = repeated_bridge();
             // Replace one reverse traversal with a detour, leaving 2 forward / 1 reverse.
             c.segs.insert(c.segs.begin() + 12, {{7, 5}, false, {}, false});
             reject_surface(c);
         }},
        {"same-direction duplication",
         [&] {
             reject_surface(
                 polygon_contour({{0, 0}, {8, 0}, {8, 8}, {0, 8}, {0, 0}, {8, 0}, {8, 8}, {0, 8}}));
         }},
        {"profile remains strict",
         [] {
             auto j = board();
             j.profile.contours = {two_bridged_holes()};
             rejected(j, {"degenerate contour"});
         }},
    });
}
void rounding_duplicates() {
    std::vector<OdbPoint> points{{0, 0}};
    for (int i = 1; i <= 40000; ++i)
        points.push_back({static_cast<double>(i) * 0.0002, 0});
    points.insert(points.end(), {{8, 8}, {0, 8}});
    const auto c = polygon_contour(points);
    auto j = board();
    j.profile.contours = {c};
    rejected(j, {"degenerate contour"});
    equivalent_surface(c, {square(8)}, 64e6, 1, 0);
}
EcadModel timed_mapping(const OdbJob& j, const char* name) {
    std::cout << "RUN " << name << std::endl;
    const auto start = std::chrono::steady_clock::now();
    auto result = map_odb(j);
    const auto stop = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(stop - start).count();
    std::cout << "MEASURE " << name << " seconds=" << seconds << std::endl;
    require(bool(result),
            "expected mapping success; got " + (result ? std::string{} : result.error().message));
    require(seconds <= 5.0, std::string(name) + " mapping exceeded 5.0 seconds");
    return result.value();
}
void transpose_equivalence() {
    constexpr std::size_t count = 6000;
    std::vector<OdbPoint> points{{10, 10}, {990, 10}};
    points.reserve(count);
    for (std::size_t i = 0; i < count - 2; ++i)
        points.push_back({990.0 - 980.0 * static_cast<double>(i) / static_cast<double>(count - 3),
                          i % 2 == 0 ? 500.0 : 501.0});
    require(points.size() == count, "exactly 6000 contour vertices");
    using Ring = std::vector<std::array<std::int64_t, 2>>;
    auto canonical = [](Ring ring) {
        require(!ring.empty(), "canonical ring is nonempty");
        std::rotate(ring.begin(), std::min_element(ring.begin(), ring.end()), ring.end());
        return ring;
    };
    auto twice_area = [](const Ring& ring) {
        std::int64_t sum = 0;
        for (std::size_t i = 0; i < ring.size(); ++i) {
            const auto& a = ring[i];
            const auto& b = ring[(i + 1) % ring.size()];
            sum += a[0] * b[1] - b[0] * a[1];
        }
        return sum;
    };
    Ring input;
    for (const auto& point : points)
        input.push_back({std::llround(point.x * 1000), std::llround(point.y * 1000)});
    auto j = board();
    j.profile.contours = {square(1000)};
    j.features["TOP"].surfaces = {{true, {polygon_contour(points)}}};
    const auto transposed = timed_mapping(j, "odbpp_map_transpose_equivalence_surface");
    const auto& rings = transposed.layers.front().polygons;
    require(rings.size() == 1, "single surface produces one comb ring");

    // The isolated pad disables transposition without changing the comb geometry.
    j.features["TOP"].symbols = {{"rect1000x1000"}};
    j.features["TOP"].pads = {{{5, 5}, 0, true}};
    const auto untransposed = timed_mapping(j, "odbpp_map_transpose_equivalence_pad");
    auto other_rings = untransposed.layers.front().polygons;
    require(other_rings.size() == 2, "comb and isolated pad produce two rings");
    const Ring pad{{4500, 4500}, {5500, 4500}, {5500, 5500}, {4500, 5500}};
    auto is_pad = [&](const auto& polygon) { return canonical(polygon.points) == pad; };
    require(std::count_if(other_rings.begin(), other_rings.end(), is_pad) == 1,
            "exactly one ring matches the isolated pad's integer square");
    std::erase_if(other_rings, is_pad);
    require(rings.size() == other_rings.size(), "scan directions preserve ring count");
    for (std::size_t i = 0; i < rings.size(); ++i) {
        require(canonical(rings[i].points) == canonical(other_rings[i].points),
                "scan directions preserve cyclic integer sequence and winding");
        require(twice_area(rings[i].points) == twice_area(other_rings[i].points),
                "scan directions preserve signed integer twice-area");
    }
    require(twice_area(rings.front().points) == twice_area(input),
            "comb output twice-area equals rounded input shoelace sum exactly");
}
void large_contour_perf() {
    constexpr std::size_t count = 200000;
    std::vector<OdbPoint> points{{10, 10}, {990, 10}};
    points.reserve(count);
    for (std::size_t i = 0; i < count - 2; ++i)
        points.push_back({990.0 - 980.0 * static_cast<double>(i) / static_cast<double>(count - 3),
                          i % 2 == 0 ? 500.0 : 501.0});
    require(points.size() == count, "exactly 200000 contour vertices");
    for (std::size_t i = 0; i < points.size(); ++i) {
        const auto& a = points[i];
        const auto& b = points[(i + 1) % points.size()];
        require(std::llround(a.x * 1000) != std::llround(b.x * 1000) ||
                    std::llround(a.y * 1000) != std::llround(b.y * 1000),
                "comb has no rounded duplicate neighbors");
    }
    auto j = board();
    j.profile.contours = {square(1000)};
    j.features["TOP"].surfaces = {{true, {polygon_contour(points)}}};
    const auto m = timed_mapping(j, "odbpp_map_large_contour_perf");
    require(m.layers.front().polygons.size() == 1, "comb remains one copper ring");
    std::vector<std::array<std::int64_t, 2>> expected;
    for (const auto& point : points)
        expected.push_back({std::llround(point.x * 1000), std::llround(point.y * 1000)});
    std::int64_t twice_area = 0;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const auto& a = expected[i];
        const auto& b = expected[(i + 1) % expected.size()];
        twice_area += a[0] * b[1] - b[0] * a[1];
    }
    near(area(m), static_cast<double>(twice_area) / 2, 0);
    auto actual = m.layers.front().polygons.front().points;
    std::sort(expected.begin(), expected.end());
    std::sort(actual.begin(), actual.end());
    require(actual == expected, "boolean scan direction preserves every integer comb vertex");
    require(m.outline.points ==
                std::vector<std::array<std::int64_t, 2>>{
                    {0, 0}, {1000000, 0}, {1000000, 1000000}, {0, 1000000}},
            "boolean scan direction leaves the profile unchanged");
}
void large_bridged_perf() {
    constexpr int side = 140;
    auto hole_y = [](int row) { return 11.0 + static_cast<double>(row) * 0.4; };
    std::vector<OdbPoint> points{{10, 10}};
    for (int column = 0; column < side; ++column) {
        const double x = 11.0 + static_cast<double>(column) * 0.4;
        points.push_back({x, 10});
        for (int row = 0; row < side; ++row) {
            const double y = hole_y(row);
            points.push_back({x, y});
            points.push_back({x, y + 0.2});
        }
        for (int row = side - 1; row >= 0; --row) {
            const double y = hole_y(row);
            points.insert(points.end(), {{x + 0.2, y + 0.2}, {x + 0.2, y}, {x, y}});
            if (row > 0)
                points.push_back({x, hole_y(row - 1) + 0.2});
        }
        points.push_back({x, 10});
    }
    points.insert(points.end(), {{90, 10}, {90, 90}, {10, 90}});
    auto j = board();
    j.profile.contours = {square(100)};
    j.features["TOP"].surfaces = {{true, {polygon_contour(points)}}};
    const auto m = timed_mapping(j, "odbpp_map_large_bridged_perf");
    near(area(m), 5616000000.0, 0);
    require(m.layers.front().polygons.size() == 19601, "exterior and 19600 holes retained");
}
} // namespace
int main() {
    const std::pair<const char*, std::function<void()>> tests[] = {
        {"odbpp_map_units", units},
        {"odbpp_map_symbol_units", symbol_units},
        {"odbpp_map_unavailable_thickness", unavailable_thickness},
        {"odbpp_map_line_width", line_width},
        {"odbpp_map_collapsed_line", collapsed_line},
        {"odbpp_map_negative", negative},
        {"odbpp_map_stackup_kicad", stackup},
        {"odbpp_map_missing_thickness", missing_thickness},
        {"odbpp_map_missing_material", missing_material},
        {"odbpp_map_oval_capsules", oval_capsules},
        {"odbpp_map_thickness_override", overrides},
        {"odbpp_map_override_provenance", override_provenance},
        {"odbpp_map_copper_weight_ignored", weight},
        {"odbpp_map_override_case", override_case},
        {"odbpp_map_skip_layers", skip},
        {"odbpp_map_unsupported_symbol", unsupported},
        {"odbpp_map_stroke_symbols", stroke_symbols},
        {"odbpp_map_unknown_drill_symbols", unknown_drill_symbols},
        {"odbpp_map_via", via},
        {"odbpp_map_via_region_initialization", via_region_initialization},
        {"odbpp_map_non_plated", non_plated},
        {"odbpp_map_mixed_tools", mixed_tools},
        {"odbpp_map_via_fraction_invariants", via_fraction_invariants},
        {"odbpp_map_via_layer_envelope", via_layer_envelope},
        {"odbpp_map_duplicate_geometry", duplicate_geometry},
        {"odbpp_map_duplicate_layer", duplicate_layer},
        {"odbpp_map_override_collision", collision},
        {"odbpp_map_holes_profile", holes_and_profile},
        {"odbpp_map_invalid_contours", invalid_contours},
        {"odbpp_map_pads_arcs", pads_and_arcs},
        {"odbpp_map_rect_strokes_rotation", rect_strokes_and_rotation},
        {"odbpp_map_boundaries", boundaries},
        {"odbpp_map_ipc_arc_equivalence", ipc_arc_equivalence},
        {"odbpp_map_rectangular_traces", rectangular_traces},
        {"odbpp_map_degenerate_copper", degenerate_copper},
        {"odbpp_map_boolean_ring_invariants", boolean_ring_invariants},
        {"odbpp_map_unknown_drill_tools", unknown_drill_tools},
        {"odbpp_map_via_radii", via_radii},
        {"odbpp_map_drill_rejections", drill_rejections},
        {"odbpp_map_bridged_surface", bridged_surface},
        {"odbpp_map_bridged_hole_chain", bridged_hole_chain},
        {"odbpp_map_bridge_rejections", bridge_rejections},
        {"odbpp_map_rounding_duplicates", rounding_duplicates},
        {"odbpp_map_via_tool_type_via", via_tool_type_via},
        {"odbpp_map_via_span_case", via_span_case},
        {"odbpp_map_transpose_equivalence", transpose_equivalence},
        {"odbpp_map_large_bridged_perf", large_bridged_perf},
        {"odbpp_map_large_contour_perf", large_contour_perf}};
    int failures = 0;
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& e) {
            ++failures;
            std::cerr << "FAIL " << name << ": " << e.what() << '\n';
        }
    }
    return failures == 0 ? 0 : 1;
}
