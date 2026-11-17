#include "chipcae/geometry/ecad/ipc2581.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>

// Reserve a private directory so concurrent test processes never share map files.
struct ScopedMap {
    std::filesystem::path directory;
    ScopedMap() {
        std::random_device random;
        do {
            directory = std::filesystem::temp_directory_path() /
                        ("ikicad-map-" + std::to_string(random()) + "-" + std::to_string(random()));
        } while (!std::filesystem::create_directory(directory));
    }
    ~ScopedMap() { std::filesystem::remove_all(directory); }
    std::filesystem::path path() const { return directory / "map.json"; }
};

namespace ecad = chipcae::geometry::ecad;
static std::string dump_model_for_golden(const ecad::EcadModel& m) {
    std::ostringstream o;
    auto d = [&](double v) {
        char b[128];
        std::snprintf(b, sizeof(b), "%a", v);
        o << b << '\n';
    };
    auto str = [&](const std::string& v) { o << std::quoted(v) << '\n'; };
    auto pos = [&](const auto& p) { o << p[0] << '\n' << p[1] << '\n'; };
    auto poly = [&](const ecad::Polygon& p) {
        o << p.points.size() << '\n';
        for (auto& v : p.points)
            pos(v);
    };
    str(m.id);
    d(m.dbu_m);
    o << m.layers.size() << '\n';
    for (auto& l : m.layers) {
        str(l.name);
        str(l.material);
        str(l.function);
        d(l.thickness_m);
        o << l.copper << '\n' << l.polygons.size() << '\n';
        for (auto& p : l.polygons)
            poly(p);
    }
    poly(m.outline);
    o << m.report.unsupported.size() << '\n';
    for (auto& u : m.report.unsupported) {
        str(u.code);
        str(u.element);
        o << u.line << '\n';
    }
    o << m.vias.size() << '\n';
    for (auto& v : m.vias) {
        str(v.id);
        str(v.padstack);
        str(v.start_layer);
        str(v.end_layer);
        pos(v.position);
        d(v.outer_radius_dbu);
        d(v.inner_radius_dbu);
        o << v.dielectric_layers.size() << '\n';
        for (auto i : v.dielectric_layers)
            o << i << '\n';
    }
    o << m.components.size() << '\n';
    for (auto& c : m.components) {
        str(c.id);
        str(c.package);
        pos(c.position);
        d(c.rotation_degrees);
        o << c.mirror << '\n';
        poly(c.outline);
        o << c.pads.size() << '\n';
        for (auto& p : c.pads) {
            str(p.id);
            pos(p.position);
        }
    }
    o << m.via_fractions.size() << '\n';
    for (auto& f : m.via_fractions) {
        o << f.layer << '\n';
        for (auto v : f.lower)
            d(v);
        for (auto v : f.upper)
            d(v);
        d(f.fraction);
        o << f.via_count << '\n';
    }
    return o.str();
}
static bool golden_regression() {
    const auto root = std::filesystem::path(CHIPCAE_TEST_SOURCE_DIR);
    const ScopedMap temporary;
    const auto map = temporary.path();
    {
        std::ofstream out(map);
        out << R"({"FR4":"mat.fr4","copper":"mat.cu"})";
    }
    for (auto board : {"board_2layer", "board_4layer", "board_arc"}) {
        auto r = ecad::read_ipc2581(root / "tests/integration/ecad/data" /
                                    (std::string(board) + ".xml"));
        if (!r)
            return false;
        for (auto stage : {"read", "mapped"}) {
            std::string error;
            if (std::string(stage) == "mapped" && !ecad::apply_material_map(r.model, map, &error)) {
                std::cerr << error << '\n';
                return false;
            }
            auto path =
                root / "tests/unit/golden" / ("ipc_" + std::string(board) + "." + stage + ".txt");
            auto actual = dump_model_for_golden(r.model);
            std::ifstream in(path);
            std::string expected((std::istreambuf_iterator<char>(in)), {});
            if (!in || expected != actual) {
                std::istringstream a(actual), e(expected);
                std::string al, el;
                std::size_t line = 0;
                for (;;) {
                    ++line;
                    const bool has_actual = bool(std::getline(a, al));
                    const bool has_expected = bool(std::getline(e, el));
                    if (has_actual != has_expected || al != el || !has_actual)
                        break;
                }
                std::cerr << path << ": first differing line " << line << " expected [" << el
                          << "] actual [" << al << "]\n";
                return false;
            }
        }
    }
    std::filesystem::remove(map);
    return true;
}

static bool kicad_acceptance() {
    const auto root =
        std::filesystem::path(CHIPCAE_TEST_SOURCE_DIR) / "tests/integration/ecad/data/kicad_like";
    int failures = 0;
    auto check = [&](bool ok, const std::string& name) {
        std::cout << (ok ? "PASS " : "FAIL ") << name << '\n';
        if (!ok)
            ++failures;
    };
    auto read = [&](const char* name) {
        auto r = ecad::read_ipc2581(root / (std::string(name) + ".xml"));
        check(bool(r), std::string(name) + " import: " + r.error);
        return r;
    };
    for (auto name : {"conductor", "conductor_late"}) {
        auto r = read(name);
        const char* names[] = {"F.Cu", "In1.Cu", "In2.Cu", "B.Cu"};
        const double thickness[] = {0.035 * 0.001, 0.018 * 0.001, 0.020 * 0.001, 0.030 * 0.001};
        check(r.model.layers.size() == 4, "ipc_kicad_conductor count");
        for (std::size_t i = 0; i < r.model.layers.size() && i < 4; ++i) {
            const auto& l = r.model.layers[i];
            check(l.name == names[i] && l.copper && l.thickness_m == thickness[i] &&
                      l.polygons.empty(),
                  std::string("ipc_kicad_conductor ") + name + " " + names[i]);
        }
    }
    for (auto name :
         {"skip_nonstructural", "unknown_function", "soldermask_b", "unsupported_order"}) {
        auto r = read(name);
        std::vector<std::string> expected;
        std::size_t count = 1;
        if (std::string(name) == "skip_nonstructural" || std::string(name) == "unsupported_order")
            expected = {"SkippedLayer:F.Silkscreen:SILKSCREEN", "SkippedLayer:F.Paste:SOLDERPASTE"};
        if (std::string(name) == "skip_nonstructural")
            expected.push_back("SkippedLayer:B.Silkscreen:SILKSCREEN");
        if (std::string(name) == "soldermask_b" || std::string(name) == "unsupported_order")
            expected.push_back("SolderMask:B.Mask");
        if (std::string(name) == "unknown_function" || std::string(name) == "unsupported_order")
            expected.push_back("LayerFunction:FOO:Mystery");
        if (std::string(name) == "unknown_function")
            count = 2;
        if (std::string(name) == "unsupported_order") {
            expected.push_back("VendorExtension");
            expected.push_back("VendorExtension");
            expected.push_back("OtherExtension");
        }
        check(r.model.layers.size() == count, std::string("ipc_kicad_") + name + " retained count");
        if (!r.model.layers.empty()) {
            const auto& l = r.model.layers.back();
            check(l.name == "DIELECTRIC_1" && !l.copper && l.thickness_m == 0.1 * 0.001 &&
                      l.polygons.empty(),
                  std::string(name) + " retained dielectric unchanged");
        }
        check(r.model.report.unsupported.size() == expected.size(),
              std::string(name) + " unsupported count/dedup");
        for (std::size_t i = 0; i < expected.size() && i < r.model.report.unsupported.size(); ++i)
            check(r.model.report.unsupported[i].element == expected[i] &&
                      r.model.report.unsupported[i].code == "IMPORT_UNSUPPORTED_ELEMENT",
                  std::string(name) + " unsupported[" + std::to_string(i) + "] expected " +
                      expected[i]);
    }
    auto matrix = read("function_matrix");
    check(matrix.model.layers.size() == 7, "function matching retained count");
    for (std::size_t i = 0; i < matrix.model.layers.size() && i < 7; ++i)
        check(matrix.model.layers[i].copper == (i < 4),
              "function matching copper " + std::to_string(i));
    const std::vector<std::string> matrix_entries = {"SolderMask:mask",
                                                     "SkippedLayer:silk:legend",
                                                     "SkippedLayer:paste:pastemask",
                                                     "SkippedLayer:document:DOCUMENT",
                                                     "SkippedLayer:assembly:ASSEMBLY",
                                                     "SkippedLayer:court:COURTYARD",
                                                     "SkippedLayer:glue:GLUE",
                                                     "SkippedLayer:outline:BOARD_OUTLINE",
                                                     "SkippedLayer:drill:DRILL_CONDUCTOR",
                                                     "LayerFunction:SOLDERMASK_TOP:unknown"};
    check(matrix.model.report.unsupported.size() == matrix_entries.size(),
          "function matching unsupported count");
    for (std::size_t i = 0; i < matrix.model.report.unsupported.size() && i < matrix_entries.size();
         ++i)
        check(matrix.model.report.unsupported[i].element == matrix_entries[i],
              "function matching " + matrix_entries[i]);
    for (auto name : {"material_precedence", "nonmaterial_spec", "missing_spec"}) {
        auto r = read(name);
        check(r.model.layers.size() == 1 &&
                  r.model.layers[0].material ==
                      (std::string(name) == "material_precedence" ? "preferred" : ""),
              std::string(name) + " source material");
    }
    const ScopedMap temporary;
    const auto map = temporary.path();
    auto mapped =
        [&](const char* fixture, const char* json, bool success, const std::string& diagnostic) {
            auto r = read(fixture);
            {
                std::ofstream out(map);
                out << json;
            }
            if (std::string(fixture) == "dielectric_by_material")
                check(r.model.layers.size() == 1 && r.model.layers[0].material == "SiO2",
                      "ipc_kicad_dielectric_by_material source Spec first nonempty Property");
            std::string error;
            bool ok = ecad::apply_material_map(r.model, map, &error);
            check(ok == success && (success ? r.model.layers.size() == 1 &&
                                                  r.model.layers[0].material == "mapped"
                                            : error.find("MATERIAL_MAPPING_MISSING") == 0 &&
                                                  error.find(diagnostic) != std::string::npos),
                  std::string("ipc_kicad_") + fixture + " map " + json + " actual: " + error);
        };
    mapped("dielectric_by_material", R"({"SiO2":"mapped"})", true, "");
    mapped("dielectric_by_material", R"({"sio2":"mapped"})", true, "");
    mapped(
        "dielectric_by_material", R"({"SiO2":"mapped","sio2":"other","SIO2":"other"})", true, "");
    mapped("dielectric_by_material",
           R"({"sio2":"a","SIO2":"b"})",
           false,
           "DIELECTRIC_1 (tried: \"SiO2\", \"layer:DIELECTRIC_1\"; case-conflict: \"SIO2\", "
           "\"sio2\")");
    mapped("dielectric_by_layer", R"({"layer:DIELECTRIC_1":"mapped"})", true, "");
    auto r = read("all_missing");
    {
        std::ofstream out(map);
        out << "{}";
    }
    std::string error;
    const bool all_mapped = ecad::apply_material_map(r.model, map, &error);
    check(!all_mapped && error.find("MATERIAL_MAPPING_MISSING: 3 layer(s) unmapped:") == 0,
          "ipc_kicad_all_missing aggregate prefix: " + error);
    for (auto text : {"DIELECTRIC_1 (tried: \"SiO2\", \"layer:DIELECTRIC_1\")",
                      "DIELECTRIC_2 (tried: \"Resin\", \"layer:DIELECTRIC_2\")",
                      "DIELECTRIC_3 (tried: \"layer:DIELECTRIC_3\")"})
        check(error.find(text) != std::string::npos, std::string("ipc_kicad_all_missing ") + text);
    std::filesystem::remove(map);
    return failures == 0;
}

// Acceptance cases deliberately collect failures so one missing feature cannot hide another.
static ecad::EcadModel geometry_read(const std::filesystem::path& path) {
    auto r = ecad::read_ipc2581(path);
    if (!r)
        throw std::runtime_error("import failed: " + r.error);
    return std::move(r.model);
}
static ecad::EcadModel geometry_xml(const std::string& body, const std::string& dictionary = "") {
    ScopedMap temporary;
    auto path = temporary.directory / "input.xml";
    {
        std::ofstream out(path);
        out << "<IPC-2581><CadHeader units='MILLIMETER'/><CadData>"
               "<Layer name='F.Cu' layerFunction='CONDUCTOR'/>"
               "<Stackup><StackupGroup><StackupLayer layerOrGroupRef='F.Cu' thickness='0.035'/>"
               "</StackupGroup></Stackup>"
            << dictionary << "<Step name='acceptance'><LayerFeature layerRef='F.Cu'><Set>" << body
            << "</Set></LayerFeature></Step></CadData></IPC-2581>";
    }
    return geometry_read(path);
}
static std::string geometry_line(int y, const std::string& desc) {
    return "<Line startX='1' startY='" + std::to_string(y) + "' endX='11' endY='" +
           std::to_string(y) + "'>" + desc + "</Line>";
}
static double geometry_area(const ecad::EcadModel& m, const std::string& name) {
    double result = 0;
    for (const auto& l : m.layers) {
        if (l.name != name)
            continue;
        for (const auto& p : l.polygons) {
            long double twice = 0;
            for (std::size_t i = 0; i < p.points.size(); ++i) {
                auto a = p.points[i], b = p.points[(i + 1) % p.points.size()];
                twice +=
                    static_cast<long double>(a[0]) * b[1] - static_cast<long double>(b[0]) * a[1];
            }
            result += std::abs(static_cast<double>(twice)) * m.dbu_m * m.dbu_m / 2;
        }
    }
    return result * 1e6; // mm^2
}
static bool geometry_acceptance() {
    const auto root =
        std::filesystem::path(CHIPCAE_TEST_SOURCE_DIR) / "tests/integration/ecad/data/kicad_like";
    const double pi = std::acos(-1.0);
    auto round_area = [&](double length, double width) {
        return length * width + pi * width * width / 4;
    };
    int failures = 0;
    std::string current;
    auto check = [&](bool ok, const std::string& detail) {
        if (!ok) {
            ++failures;
            std::cerr << "FAIL " << current << ": " << detail << '\n';
        }
    };
    auto area = [&](const ecad::EcadModel& m, double expected, const std::string& layer = "F.Cu") {
        const double actual = geometry_area(m, layer);
        check(std::abs(actual - expected) / expected <= 1e-3,
              layer + " area expected=" + std::to_string(expected) +
                  " actual=" + std::to_string(actual));
    };
    auto reports =
        [&](const ecad::EcadModel& m, std::set<std::string> expected, bool via_only = false) {
            std::set<std::string> actual;
            std::size_t count = 0;
            for (const auto& u : m.report.unsupported) {
                if (via_only && u.element.rfind("Via", 0) != 0)
                    continue;
                ++count;
                actual.insert(u.element);
                check(u.code == "IMPORT_UNSUPPORTED_ELEMENT", "unsupported code");
            }
            std::ostringstream detail;
            detail << "unsupported expected";
            for (const auto& e : expected)
                detail << " [" << e << ']';
            detail << " actual count=" << count;
            for (const auto& e : actual)
                detail << " [" << e << ']';
            check(actual == expected && count == expected.size(), detail.str());
        };
    auto run = [&](const char* name, auto test) {
        current = name;
        const int before = failures;
        try {
            test();
        } catch (const std::exception& e) {
            check(false, e.what());
        }
        std::cout << (before == failures ? "PASS " : "FAIL ") << name << '\n';
    };
    run("ipc_duplicate_lines_union", [&] {
        const auto line = geometry_line(1, "<LineDesc lineWidth='0.4'/>");
        auto model = geometry_xml(line + line);
        check(model.layers.size() == 1 && model.layers[0].polygons.size() == 1,
              "identical lines merge into one polygon");
        area(model, round_area(10, .4));
        reports(model, {});
    });
    run("ipc_pad_artwork_before_stackup", [&] {
        ScopedMap temporary;
        const auto path = temporary.directory / "forward-pad.xml";
        {
            std::ofstream out(path);
            out << "<IPC-2581><CadHeader units='MILLIMETER'/><CadData>"
                   "<EntryStandard id='disk'><Circle diameter='1'/></EntryStandard>"
                   "<Step><LayerFeature layerRef='F.Cu'><Set>"
                   "<Pad padstackDefRef='artwork'><Location x='2' y='3'/>"
                   "<StandardPrimitiveRef id='disk'/></Pad>"
                << geometry_line(3, "<LineDesc lineWidth='0.2'/>")
                << "</Set></LayerFeature></Step>"
                   "<StackupLayer layerOrGroupRef='mask'/>"
                   "<StackupLayer layerOrGroupRef='F.Cu'/>"
                   "<Layer name='mask' layerFunction='SOLDERMASK'/>"
                   "<Layer name='F.Cu' layerFunction='CONDUCTOR'/>"
                   "</CadData></IPC-2581>";
        }
        auto model = geometry_read(path);
        check(model.layers.size() == 1 && model.layers[0].name == "F.Cu" &&
                  model.layers[0].polygons.size() == 1,
              "forward pad and trace merge on compacted copper layer");
        // Compare with the same artwork parsed after its layer has been indexed.
        auto baseline =
            geometry_xml("<Pad padstackDefRef='artwork'><Location x='2' y='3'/>"
                         "<StandardPrimitiveRef id='disk'/></Pad>" +
                             geometry_line(3, "<LineDesc lineWidth='0.2'/>"),
                         "<EntryStandard id='disk'><Circle diameter='1'/></EntryStandard>");
        area(model, geometry_area(baseline, "F.Cu"));
        check(geometry_area(model, "F.Cu") > round_area(10, .2),
              "forward pad contributes copper area");
        check(model.vias.empty(), "pad artwork creates no drilled vias");
    });
    run("ipc_pad_union_preserves_unrelated_trace", [&] {
        const auto artwork = "<Pad padstackDefRef='artwork'><Location x='0' "
                             "y='0'/><StandardPrimitiveRef id='disk'/></Pad>" +
                             geometry_line(5, "<LineDesc lineWidth='0.4'/>") +
                             "<Line startX='0' startY='0' endX='0.2' endY='0'>"
                             "<LineDesc lineWidth='0.2'/></Line>";
        const std::string dictionary =
            "<EntryStandard id='disk'><Circle diameter='1'/></EntryStandard>";
        const auto baseline = geometry_xml(artwork, dictionary);
        const auto filtered = geometry_xml(
            artwork + "<Polygon/>"
                      "<Polygon><PolyBegin x='2' y='2'/></Polygon>"
                      "<Polygon><PolyBegin x='2' y='2'/><PolyStepSegment x='3' y='3'/></Polygon>"
                      "<Polygon><PolyBegin x='2' y='2'/><PolyStepSegment x='3' y='3'/>"
                      "<PolyStepSegment x='4' y='4'/></Polygon>"
                      "<Polygon><PolyBegin x='2' y='2'/><PolyStepSegment x='2.001' y='2'/>"
                      "<PolyStepSegment x='2' y='2.001'/></Polygon>",
            dictionary);
        for (const auto* model : {&baseline, &filtered}) {
            check(model->layers.size() == 1 && model->layers[0].polygons.size() == 2,
                  "pad and unrelated trace remain separate");
            area(*model, pi / 4 + round_area(10, .4));
            if (model->layers.size() == 1) {
                for (const auto& polygon : model->layers[0].polygons) {
                    check(polygon.points.size() >= 3, "union emits valid rings");
                    if (polygon.points.size() < 3)
                        continue;
                    auto isolated = *model;
                    isolated.layers[0].polygons = {polygon};
                    area(isolated, polygon.points.front()[1] > 1000 ? round_area(10, .4) : pi / 4);
                }
            }
        }
    });
    run("ipc_inline_linedesc_with_id", [&] {
        auto m = geometry_read(root / "inline_linedesc_with_id.xml");
        check(m.layers.size() == 1 && m.layers[0].polygons.size() == 2, "both lines appear");
        area(m, 2 * round_area(10, .4));
        if (m.layers.size() == 1 && m.layers[0].polygons.size() == 2) {
            auto isolated = m;
            for (const auto& polygon : m.layers[0].polygons) {
                isolated.layers[0].polygons = {polygon};
                area(isolated, round_area(10, .4));
            }
        }
        reports(m, {});
    });
    run("ipc_inline_priority_and_zero_width", [&] {
        auto m =
            geometry_xml(geometry_line(1, "<LineDesc lineWidth='0.4'/><LineDescRef id='W'/>") +
                             geometry_line(3, "<LineDescRef id='W'/><LineDesc lineWidth='0'/>") +
                             geometry_line(5, "<LineDesc/>"),
                         "<LineDesc id='W' lineWidth='0.2'/>");
        area(m, round_area(10, .4));
        reports(m, {"LineWidthMissing:F.Cu"});
    });
    run("ipc_round_coincident_endpoints", [&] {
        auto m = geometry_xml("<Line startX='1' startY='1' endX='1' endY='1'>"
                              "<LineDesc lineWidth='0.4' lineEnd='round'/></Line>");
        area(m, round_area(0, .4));
        reports(m, {});
    });
    run("ipc_linedesc_forward_reference", [&] {
        auto m = geometry_xml(geometry_line(1, "<LineDescRef id='LATER'/>") +
                              geometry_line(3, "<LineDesc id='LATER' lineWidth='0.4'/>"));
        area(m, 2 * round_area(10, .4));
        reports(m, {});
    });
    run("ipc_report_first_problem_order", [&] {
        auto m = geometry_xml(geometry_line(1, "") + "<VendorExtension/>" + geometry_line(3, "") +
                              geometry_line(5, "<LineDescRef id='NOPE'/>"));
        reports(m, {"LineWidthMissing:F.Cu", "VendorExtension", "LineDescMissing:NOPE"});
        check(m.report.unsupported.size() == 3 &&
                  m.report.unsupported[0].element == "LineWidthMissing:F.Cu" &&
                  m.report.unsupported[1].element == "VendorExtension" &&
                  m.report.unsupported[2].element == "LineDescMissing:NOPE",
              "deferred reports retain first-problem order");
    });
    run("ipc_line_width_missing", [&] {
        auto m = geometry_xml(geometry_line(1, "") + geometry_line(3, "") +
                              geometry_line(5, "<LineDesc id='valid' lineWidth='0.4'/>"));
        check(m.layers.size() == 1 && m.layers[0].polygons.size() == 1, "only valid line appears");
        area(m, round_area(10, .4));
        reports(m, {"LineWidthMissing:F.Cu"});
    });
    run("ipc_linedesc_duplicate", [&] {
        auto m = geometry_xml(
            geometry_line(1, "<LineDesc id='W1' lineWidth='0.4'/>") +
                geometry_line(3, "<LineDescRef id='W1'/>") +
                geometry_line(5, "<LineDesc id='W1' lineWidth='0.6'/>"),
            "<DictionaryLineDesc><LineDesc id='W1' lineWidth='0.2'/></DictionaryLineDesc>");
        area(m, round_area(10, .4) + round_area(10, .2) + round_area(10, .6));
        // The dictionary wrapper remains an unrelated unsupported XML element.
        reports(m, {"DictionaryLineDesc", "LineDescDuplicate:W1"});
    });
    run("ipc_linedesc_missing", [&] {
        auto m = geometry_xml(geometry_line(1, "<LineDescRef id='NOPE'/>") +
                              geometry_line(3, "<LineDescRef id='NOPE'/>"));
        check(m.layers.size() == 1 && m.layers[0].polygons.empty(),
              "missing references draw no polygons");
        reports(m, {"LineDescMissing:NOPE"});
    });
    for (const auto& end : {std::string("square"),
                            std::string("flat"),
                            std::string("unknown"),
                            std::string("round")}) {
        run(("ipc_line_end_" + end).c_str(), [&] {
            auto m =
                geometry_read(root / ("line_end_" + (end == "unknown" ? "foo" : end) + ".xml"));
            area(m, end == "square" ? 10.4 * .4 : end == "flat" ? 10 * .4 : round_area(10, .4));
            reports(m,
                    end == "unknown" ? std::set<std::string>{"LineEndUnsupported:F.Cu:FOO"}
                                     : std::set<std::string>{});
        });
    }
    for (bool square : {false, true}) {
        run(square ? "ipc_arc_square_as_round" : "ipc_arc_round", [&] {
            auto m = geometry_read(root / (square ? "arc_square.xml" : "arc_round.xml"));
            area(m, pi * 100 * 2 + pi);
            reports(m,
                    square ? std::set<std::string>{"LineEndUnsupported:F.Cu:SQUARE"}
                           : std::set<std::string>{});
        });
    }
    run("ipc_kicad_inline_linedesc", [&] {
        auto m = geometry_read(root / "board_kicad10/board.xml");
        check(m.layers.size() == 7, "four copper and three dielectric layers");
        for (const auto& name : {"F.Cu", "B.Cu"}) {
            bool nonempty = false;
            for (const auto& l : m.layers)
                if (l.name == name)
                    nonempty = !l.polygons.empty();
            check(nonempty, std::string(name) + " polygons > 0");
            area(m,
                 2 * round_area(10, .2) + round_area(10, .4) +
                     (std::string(name) == "F.Cu" ? 2 : 1) * pi / 4,
                 name);
        }
        for (const auto& u : m.report.unsupported)
            check(u.element.rfind("LineWidthMissing:", 0) != 0, "no missing-width report");
    });
    run("ipc_via_plating_missing_grouped", [&] {
        auto m = geometry_read(root / "board_kicad10/board.xml");
        check(m.vias.empty() && m.via_fractions.empty(),
              "missing plating contributes no vias or fractions");
        reports(m, {"ViaPlatingMissing:F.Cu:B.Cu:1", "ViaPlatingMissing:F.Cu:In1.Cu:1"}, true);
    });
    run("ipc_via_plating_mixed", [&] {
        auto m = geometry_read(root / "via_plating_mixed.xml");
        check(m.vias.size() == 3, "exactly three plated vias");
        for (const auto& f : m.via_fractions)
            check(f.via_count == 3, "only plated vias contribute fractions");
        reports(m, {"ViaPlatingMissing:F.Cu:B.Cu:2"});
    });
    return failures == 0;
}

int main() {
    if (!golden_regression())
        return 11;
    std::cout << "PASS ipc_linedesc_ref_unchanged\n";
    namespace ecad = chipcae::geometry::ecad;
    const auto root =
        std::filesystem::path(CHIPCAE_TEST_SOURCE_DIR) / "tests/integration/ecad/data";
    auto a = ecad::read_ipc2581(root / "board_2layer.xml");
    if (!a || a.model.layers.size() != 3 || a.model.outline.points.size() != 4)
        return 1;
    const double expected[] = {35e-6, 1.6e-3, 35e-6};
    for (std::size_t i = 0; i < 3; ++i)
        if (std::abs(a.model.layers[i].thickness_m - expected[i]) > 1e-15)
            return 2;
    if (a.model.layers[0].polygons.size() != 1 || a.model.layers[2].polygons.size() != 1)
        return 6;
    auto area = [](const ecad::Polygon& p, double dbu) {
        long double sum = 0;
        for (std::size_t i = 0; i < p.points.size(); ++i) {
            auto a = p.points[i], b = p.points[(i + 1) % p.points.size()];
            sum += static_cast<long double>(a[0]) * b[1] - static_cast<long double>(b[0]) * a[1];
        }
        return std::abs(static_cast<double>(sum)) * dbu * dbu / 2;
    };
    if (std::abs(area(a.model.layers[0].polygons[0], a.model.dbu_m) - 6e-6) > 1e-14)
        return 7;
    auto u = ecad::read_ipc2581(root / "unsupported.xml");
    if (!u || u.model.report.unsupported.size() != 3)
        return 3;
    auto b = ecad::read_ipc2581(root / "board_4layer.xml");
    if (!b || b.model.layers.size() != 5 || b.model.layers[0].polygons.empty())
        return 4;
    const auto trace_area = area(b.model.layers[0].polygons.front(), b.model.dbu_m);
    const double analytic_trace_area = (3.0 * 0.1 + std::acos(-1.0) * 0.05 * 0.05) * 1e-6;
    if (std::abs(trace_area - analytic_trace_area) / analytic_trace_area > 1e-4)
        return 10;
    auto c = ecad::read_ipc2581(root / "board_arc.xml");
    if (!c || c.model.layers.size() != 1 || c.model.layers[0].polygons.empty())
        return 5;
    const ScopedMap output_directory;
    const auto output = output_directory.directory;
    std::filesystem::create_directories(output);
    std::string error;
    if (!ecad::to_stack(a.model, output / "board.stack.json", output / "board.polyset", &error))
        return 8;
    if (!std::filesystem::exists(output / "board.stack.json") ||
        std::filesystem::file_size(output / "board.polyset") == 0)
        return 9;
    std::filesystem::remove_all(output);
    if (!kicad_acceptance())
        return 12;
    if (!geometry_acceptance())
        return 13;
    std::cout << "IPC-2581 SAX reader checks passed\n";
    return 0;
}
