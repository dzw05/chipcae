#include "map.hpp"

#include <algorithm>
#include <array>
#include <clipper2/clipper.h>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <numbers>
#include <random>
#include <set>
#include <sstream>
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
void missing_material() {
    auto j = board();
    j.attrs.layer["CORE"].erase(".material");
    rejected(j, {".material", "CORE"});
    j.attrs.layer["CORE"][".material"] = "";
    rejected(j, {".material", "CORE"});
    j.attrs.layer.erase("CORE");
    rejected(j, {".material", "CORE"});
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
        require(r.source == (i == 3 ? "odb" : "override"),
                "override filename source and ODB precedence");
        require(r.odb_value_m.has_value() == (i == 3), "conflict provenance only");
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
    auto j = board();
    auto& f = j.features["TOP"];
    f.symbols = {{"unknown_symbol"}};
    f.pads.push_back({{4, 4}, 0, true});
    auto m = mapped(j);
    require(m.report.unsupported.size() == 1, "one unknown symbol report");
    require(m.report.unsupported.front().element == "Symbol:TOP:0", "unknown symbol identity");
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
}
void via_locality() {
    auto j = board();
    OdbLayer drill;
    drill.name = "DRILL";
    drill.type = "drill";
    drill.start_name = "TOP";
    drill.end_name = "BOTTOM";
    j.layers.push_back(drill);
    j.attrs.layer["DRILL"][".plating_thickness"] = "0.025";
    auto& f = j.features["DRILL"];
    f.symbols = {{"r600"}};
    for (bool overlap : {false, true}) {
        f.pads = {{{2, 3}, 0, true}, {{overlap ? 2.2 : 200, 3}, 0, true}};
        if (overlap)
            f.pads.push_back(f.pads.front()); // duplicate bounds must not duplicate cells
        const auto m = mapped(j);
        require(m.via_fractions.size() == 2, "two local cells");
        double integrated = 0;
        for (std::size_t k = 0; k < m.via_fractions.size(); ++k) {
            const auto& cell = m.via_fractions[k];
            require(cell.layer == 1 && cell.fraction > 0 && cell.fraction <= 1,
                    "dielectric cell is rasterized");
            bool local = false;
            for (const auto& v : m.vias)
                local = local || (cell.lower[0] >= v.position[0] - v.outer_radius_dbu &&
                                  cell.upper[0] <= v.position[0] + v.outer_radius_dbu &&
                                  cell.lower[1] >= v.position[1] - v.outer_radius_dbu &&
                                  cell.upper[1] <= v.position[1] + v.outer_radius_dbu);
            require(local, "every cell lies inside one via's bounding box");
            require(cell.via_count == (overlap ? (k == 0 ? 3 : 1) : 1), "local via count");
            for (std::size_t n = 0; n < k; ++n) {
                const auto& prior = m.via_fractions[n];
                require(cell.upper[0] <= prior.lower[0] || prior.upper[0] <= cell.lower[0] ||
                            cell.upper[1] <= prior.lower[1] || prior.upper[1] <= cell.lower[1],
                        "fraction cells have disjoint interiors");
            }
            integrated +=
                cell.fraction * (cell.upper[0] - cell.lower[0]) * (cell.upper[1] - cell.lower[1]);
        }
        ViaFraction reference;
        reference.layer = 1;
        reference.lower = {1700, 2700};
        reference.upper = {overlap ? 2500.0 : 200300.0, 3300};
        std::vector<ViaFraction> cells{reference};
        require(rasterize_vias(m, cells), "union reference rasterizes");
        near(integrated, cells[0].fraction * (reference.upper[0] - reference.lower[0]) * 600, 1e-6);
        if (!overlap)
            near(integrated, 2 * std::numbers::pi * (300 * 300 - 275 * 275), 1e-6);
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
    f.symbols = {{"r600"}};
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
// Independently construct the radius-based quarter-arc discretization in MM.
// Compare exact offset vertices, including joins, rather than an analytic area bound.
void check_arc_stroke(const OdbJob& j, bool rectangular) {
    using namespace Clipper2Lib;
    const auto& arc = j.features.at("TOP").arcs.front();
    const double radius = 2;
    const double tolerance = 0.01 * radius;
    const double step = 2 * std::acos(1 - tolerance / radius);
    const int count = static_cast<int>(std::ceil((std::numbers::pi / 2) / step));
    const double begin = std::atan2(arc.s.y - arc.c.y, arc.s.x - arc.c.x);
    Path64 path;
    for (int i = 0; i <= count; ++i) {
        const double angle = begin + (std::numbers::pi / 2) * i / count;
        path.push_back({std::llround((arc.c.x + radius * std::cos(angle)) * 1000),
                        std::llround((arc.c.y + radius * std::sin(angle)) * 1000)});
    }
    ClipperOffset offset;
    offset.AddPath(path,
                   rectangular ? JoinType::Miter : JoinType::Round,
                   rectangular ? EndType::Square : EndType::Round);
    Paths64 expected;
    offset.Execute(500, expected);
    const auto m = mapped(j);
    require(expected.size() == 1 && m.layers.front().polygons.size() == 1,
            "one arc stroke contour");
    std::set<std::array<std::int64_t, 2>> vertices;
    for (const auto& p : expected.front())
        vertices.insert({p.x, p.y});
    const auto& actual = m.layers.front().polygons.front().points;
    require(actual.size() == expected.front().size() &&
                std::set<std::array<std::int64_t, 2>>(actual.begin(), actual.end()) == vertices,
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
    for (int orientation : {0, 1, 2, 3}) {
        const bool rotated = orientation % 2;
        const bool semicircle = orientation >= 2;
        auto j = board();
        auto& f = j.features["TOP"];
        f.surfaces.clear();
        f.symbols = {{"round1000"}};
        const double radius = semicircle ? 0.5 : 2;
        const OdbPoint start = rotated ? OdbPoint{2, 2 + radius} : OdbPoint{2 + radius, 2};
        const OdbPoint end = semicircle
                                 ? (rotated ? OdbPoint{2, 2 - radius} : OdbPoint{2 - radius, 2})
                                 : (rotated ? OdbPoint{2 - radius, 2} : OdbPoint{2, 2 + radius});
        f.arcs = {{start, end, {2, 2}, 0, true, false}};
        std::ostringstream xml;
        xml << R"(<IPC-2581><Ecad><CadData><Layer name="TOP" layerFunction="CONDUCTOR"/>
<Stackup><StackupGroup name="S"><StackupLayer layerOrGroupRef="TOP" thickness="0.035"/>
</StackupGroup></Stackup><Step name="board"><LayerFeature layerRef="TOP"><Set><Arc startX=")"
            << start.x << "\" startY=\"" << start.y << "\" endX=\"" << end.x << "\" endY=\""
            << end.y
            << R"(" centerX="2" centerY="2" clockwise="false"><LineDesc lineWidth="1" lineEnd="ROUND"/>
</Arc></Set></LayerFeature></Step></CadData></Ecad></IPC-2581>)";
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
        const auto& a = odb.layers.front().polygons.front().points;
        const auto& b = ipc.model.layers.front().polygons.front().points;
        require(a.size() == b.size() &&
                    std::set<std::array<std::int64_t, 2>>(a.begin(), a.end()) ==
                        std::set<std::array<std::int64_t, 2>>(b.begin(), b.end()),
                "ODB and IPC arc vertices agree exactly");
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
    near(m.layers.front().thickness_m, 35e-6);
    near(records.front().thickness_m, 35e-6);
    require(records.front().source == "odb" && records.front().odb_value_m == 35e-6,
            "conflicting override retains ODB and records original value");
    o["top"] = -1;
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
void via_radii() {
    auto j = board();
    OdbLayer l;
    l.name = "DRILL";
    l.type = "drill";
    l.start_name = "TOP";
    l.end_name = "BOTTOM";
    j.layers.push_back(l);
    auto& f = j.features["DRILL"];
    f.symbols = {{"r600"}};
    f.pads = {{{2, 3}, 0, true}};
    for (bool with_tool : {false, true}) {
        if (with_tool)
            j.tools["DRILL"] = {{1, "plated", 0.6}};
        for (const auto* plating : {"0.025", "0.3"}) {
            j.attrs.layer["DRILL"][".plating_thickness"] = plating;
            auto m = mapped(j);
            require(m.vias.size() == 1, "valid annulus retained");
            const auto& v = m.vias.front();
            require(0 <= v.inner_radius_dbu && v.inner_radius_dbu < v.outer_radius_dbu,
                    "valid via radius invariant");
        }
        for (const auto* plating : {"0.301", "1e-300"}) {
            j.attrs.layer["DRILL"][".plating_thickness"] = plating;
            auto m = mapped(j);
            require(m.vias.empty() && m.via_fractions.empty(), "invalid annulus omitted");
            require(m.report.unsupported.size() == 1 &&
                        m.report.unsupported.front().element == "ViaGeometry:DRILL",
                    "invalid annulus reported");
        }
    }
    f.symbols = {{"r0.5"}};
    j.attrs.layer["DRILL"][".plating_thickness"] = "0.0001";
    for (double diameter : {0.0, -0.0001}) {
        j.tools["DRILL"] = {{1, "plated", diameter}};
        auto m = mapped(j);
        require(m.vias.empty() && m.report.unsupported.size() == 1 &&
                    m.report.unsupported.front().element == "ViaGeometry:DRILL",
                "invalid matched tool diameter rejected");
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
    j.features["DRILL"].symbols = {{"r600"}};
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
    j.attrs.layer["DRILL"][".plating_thickness"] = "0.025";
    j.layers.back().end_name.clear();
    m = mapped(j);
    require(m.vias.empty() && m.report.unsupported.size() == 1 &&
                m.report.unsupported.front().element == "ViaSpan:DRILL",
            "no default span");
}
} // namespace
int main() {
    const std::pair<const char*, std::function<void()>> tests[] = {
        {"odbpp_map_units", units},
        {"odbpp_map_line_width", line_width},
        {"odbpp_map_negative", negative},
        {"odbpp_map_stackup_kicad", stackup},
        {"odbpp_map_missing_thickness", missing_thickness},
        {"odbpp_map_missing_material", missing_material},
        {"odbpp_map_oval_capsules", oval_capsules},
        {"odbpp_map_thickness_override", overrides},
        {"odbpp_map_copper_weight_ignored", weight},
        {"odbpp_map_override_case", override_case},
        {"odbpp_map_skip_layers", skip},
        {"odbpp_map_unsupported_symbol", unsupported},
        {"odbpp_map_via", via},
        {"odbpp_map_via_locality", via_locality},
        {"odbpp_map_non_plated", non_plated},
        {"odbpp_map_duplicate_geometry", duplicate_geometry},
        {"odbpp_map_duplicate_layer", duplicate_layer},
        {"odbpp_map_override_collision", collision},
        {"odbpp_map_holes_profile", holes_and_profile},
        {"odbpp_map_pads_arcs", pads_and_arcs},
        {"odbpp_map_rect_strokes_rotation", rect_strokes_and_rotation},
        {"odbpp_map_boundaries", boundaries},
        {"odbpp_map_ipc_arc_equivalence", ipc_arc_equivalence},
        {"odbpp_map_rectangular_traces", rectangular_traces},
        {"odbpp_map_degenerate_copper", degenerate_copper},
        {"odbpp_map_via_radii", via_radii},
        {"odbpp_map_drill_rejections", drill_rejections}};
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
