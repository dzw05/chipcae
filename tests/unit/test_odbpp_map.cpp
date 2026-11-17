#include "map.hpp"

#include <cmath>
#include <functional>
#include <iostream>
#include <numbers>
#include <set>
#include <stdexcept>
#include <tuple>

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
    require(bool(r), "expected mapping success; got " + r.error);
    return r.model;
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
    require(r.error.find("IMPORT_REJECTED") != std::string::npos, "wrong error code");
    for (const auto& n : names)
        require(r.error.find(n) != std::string::npos, "error missing " + n + ": " + r.error);
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
}
void line_width() {
    auto j = board();
    auto& f = j.features["TOP"];
    f.surfaces.clear();
    f.symbols = {{"r1000"}};
    f.lines.push_back({{1, 1}, {7, 1}, 0, true});
    near(area(mapped(j)), 6e6 + std::numbers::pi * 250000, 1e-3);
}
void negative() {
    auto j = board();
    auto& f = j.features["TOP"];
    f.symbols = {{"r2000"}};
    f.pads.push_back({{4, 4}, 0, false});
    near(area(mapped(j)), 64e6 - std::numbers::pi * 1e6, 1e-3);
}
void stackup() {
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
void missing_thickness() {
    rejected(missing(), {"3", "TOP", "CORE", "BOTTOM"});
}
// Acceptance requires an exact filename source. The frozen MapOptions currently
// cannot carry this filename; keep this contract gap visible until resolved.
void overrides() {
    ThicknessOverrides o = {
        {"TOP", 35e-6}, {"CORE", 800e-6}, {"BOTTOM", 35e-6}, {"PREPREG", 300e-6}};
    std::vector<ThicknessRecord> records;
    auto m = mapped(missing(), {&o, &records});
    require(records.size() == 4, "one record per layer");
    for (std::size_t i = 0; i < records.size(); ++i) {
        const auto& r = records[i];
        require(r.layer == m.layers[i].name, "record layer");
        near(r.thickness_m, o.at(r.layer));
        near(m.layers[i].thickness_m, r.thickness_m);
        require(r.source == "override:thickness.json", "override filename source");
    }
    require(records[3].odb_value_m.has_value() &&
                std::abs(*records[3].odb_value_m - 200e-6) <= 1e-12,
            "original ODB thickness");
}
void weight() {
    auto j = board();
    j.attrs.layer["TOP"][".layer_dielectric"] = "0";
    j.attrs.layer["TOP"][".copper_weight"] = "1.0";
    rejected(j, {"TOP"});
    ThicknessOverrides o = {{"TOP", 35e-6}};
    std::vector<ThicknessRecord> records;
    mapped(j, {&o, &records});
    require(records.size() == 4 && records[0].source == "override:thickness.json",
            "weight override source");
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
    auto j = board();
    auto& f = j.features["TOP"];
    f.symbols = {{"unknown_symbol"}};
    f.pads.push_back({{4, 4}, 0, true});
    auto m = mapped(j);
    require(m.report.unsupported.size() == 1, "one unknown symbol report");
    near(area(m), 64e6, 1e-3);
}
void via() {
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
    f.symbols = {{"r600"}, {"r800"}};
    f.pads = {{{2, 3}, 0, true}, {{6, 7}, 0, true}};
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
        slot_reported = slot_reported || u.element.find("slot") != std::string::npos ||
                        u.element.find("Slot") != std::string::npos;
    require(slot_reported, "slot unsupported");
    require(!m.via_fractions.empty(), "via rasterization initialized");
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
} // namespace
int main() {
    const std::pair<const char*, std::function<void()>> tests[] = {
        {"odbpp_map_units", units},
        {"odbpp_map_line_width", line_width},
        {"odbpp_map_negative", negative},
        {"odbpp_map_stackup_kicad", stackup},
        {"odbpp_map_missing_thickness", missing_thickness},
        {"odbpp_map_thickness_override", overrides},
        {"odbpp_map_copper_weight_ignored", weight},
        {"odbpp_map_override_case", override_case},
        {"odbpp_map_skip_layers", skip},
        {"odbpp_map_unsupported_symbol", unsupported},
        {"odbpp_map_via", via},
        {"odbpp_map_duplicate_geometry", duplicate_geometry},
        {"odbpp_map_duplicate_layer", duplicate_layer},
        {"odbpp_map_override_collision", collision}};
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
