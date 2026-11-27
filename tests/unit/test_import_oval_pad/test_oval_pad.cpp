#define CHIPCAE_ENABLE_ECAD 1
#include "../../../kernel/src/import_ecad_defects.hpp"

#include <algorithm>
#include <array>
#include <clipper2/clipper.h>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace {
namespace ecad = chipcae::geometry::ecad;
namespace geometry = chipcae::geometry;
namespace clip = Clipper2Lib;
using Point = std::array<std::int64_t, 2>;
const double pi = std::acos(-1.0);
const std::filesystem::path root = CHIPCAE_SOURCE_DIR;

void check(bool condition, const std::string& message) {
    if (!condition)
        throw std::runtime_error(message);
}
struct Temporary {
    std::filesystem::path path;
    Temporary() {
        std::random_device random;
        do {
            path = std::filesystem::temp_directory_path() /
                   ("oval-pad-" + std::to_string(random()) + "-" + std::to_string(random()));
        } while (!std::filesystem::create_directory(path));
    }
    ~Temporary() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
    std::filesystem::path write(const std::string& xml) const {
        const auto file = path / "input.xml";
        std::ofstream out(file, std::ios::binary);
        out << xml;
        check(static_cast<bool>(out), "write synthetic XML");
        return file;
    }
};
std::string bytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    check(static_cast<bool>(in), "read fixture");
    return {(std::istreambuf_iterator<char>(in)), {}};
}
ecad::EcadModel read(const std::filesystem::path& file) {
    auto result = ecad::read_ipc2581(file);
    check(static_cast<bool>(result), "read_ipc2581: " + result.error);
    return std::move(result.model);
}
ecad::EcadModel parse(const std::string& xml) {
    Temporary tmp;
    return read(tmp.write(xml));
}
std::string pad(const std::string& transform = "",
                bool after = false,
                const std::string& y = "-20",
                const std::string& x = "10") {
    const std::string ref = "<StandardPrimitiveRef id=\"shape\"/>\n";
    return "<Pad padstackDefRef=\"PS\">\n<Location x=\"" + x + "\" y=\"" + y + "\"/>\n" +
           (after ? ref + transform : transform + ref) + "</Pad>\n";
}
std::string board(const std::string& shape = "<Oval width=\"3\" height=\"4\"/>",
                  const std::string& pads = pad(),
                  bool mask = false,
                  const std::string& extra = "") {
    return "<IPC-2581>\n<CadHeader units=\"MILLIMETER\"/>\n"
           "<DictionaryStandard>\n<EntryStandard id=\"shape\">\n" +
           shape +
           "\n</EntryStandard>\n</DictionaryStandard>\n<CadData>\n"
           "<Layer name=\"Cu\" layerFunction=\"CONDUCTOR\"/>\n"
           "<Layer name=\"Mask\" layerFunction=\"SOLDERMASK\"/>\n"
           "<Stackup><StackupGroup>\n"
           "<StackupLayer layerOrGroupRef=\"Cu\" thickness=\"0.035\" matDes=\"FR4\"/>\n"
           "<StackupLayer layerOrGroupRef=\"Mask\" thickness=\"0.01\" matDes=\"FR4\"/>\n"
           "</StackupGroup></Stackup>\n<Step name=\"oval\">\n"
           "<Profile><Polygon><PolyBegin x=\"0\" y=\"-40\"/>"
           "<PolyStepSegment x=\"40\" y=\"-40\"/><PolyStepSegment x=\"40\" y=\"0\"/>"
           "<PolyStepSegment x=\"0\" y=\"0\"/></Polygon></Profile>\n"
           "<LayerFeature layerRef=\"" +
           (mask ? "Mask" : "Cu") + "\"><Set>\n" + pads + "</Set></LayerFeature>\n" + extra +
           "</Step></CadData></IPC-2581>\n";
}
const ecad::EcadLayer& layer(const ecad::EcadModel& model, const std::string& name = "Cu") {
    const auto found = std::find_if(model.layers.begin(),
                                    model.layers.end(),
                                    [&](const auto& item) { return item.name == name; });
    check(found != model.layers.end(), "missing layer " + name);
    return *found;
}
const ecad::Polygon& single(const ecad::EcadModel& model) {
    check(model.vias.empty(), "artwork must not create drilled vias");
    const auto& polygons = layer(model).polygons;
    check(polygons.size() == 1,
          "expected one modeled Oval copper polygon, got " + std::to_string(polygons.size()));
    return polygons.front();
}
double signed_area(const ecad::Polygon& polygon) {
    long double sum = 0;
    for (std::size_t i = 0; i < polygon.points.size(); ++i) {
        const auto& a = polygon.points[i];
        const auto& b = polygon.points[(i + 1) % polygon.points.size()];
        sum += static_cast<long double>(a[0]) * b[1] - static_cast<long double>(a[1]) * b[0];
    }
    return static_cast<double>(sum / 2);
}
double area(const ecad::Polygon& polygon) {
    return std::abs(signed_area(polygon));
}
double area(const ecad::EcadLayer& item) {
    double result = 0;
    for (const auto& polygon : item.polygons)
        result += signed_area(polygon);
    return std::abs(result);
}
double epsilon(double w, double h) {
    const double r = std::min(w, h) / 2, s = (std::max(w, h) - std::min(w, h)) / 2;
    const double a = 4 * s * r + pi * r * r, perimeter = 4 * s + 2 * pi * r;
    return 1 - (128 / pi) * std::sin(pi / 128) + perimeter * std::sqrt(2.0) / (2 * a);
}
void bounds(const ecad::Polygon& polygon, double x, double y, double hx, double hy) {
    std::array<double, 4> actual{std::numeric_limits<double>::max(),
                                 std::numeric_limits<double>::lowest(),
                                 std::numeric_limits<double>::max(),
                                 std::numeric_limits<double>::lowest()};
    for (const auto& p : polygon.points) {
        actual[0] = std::min(actual[0], static_cast<double>(p[0]));
        actual[1] = std::max(actual[1], static_cast<double>(p[0]));
        actual[2] = std::min(actual[2], static_cast<double>(p[1]));
        actual[3] = std::max(actual[3], static_cast<double>(p[1]));
    }
    const std::array<double, 4> expected{x - hx, x + hx, y - hy, y + hy};
    for (std::size_t i = 0; i < actual.size(); ++i)
        check(std::abs(actual[i] - expected[i]) <= 1, "Oval bbox coordinate " + std::to_string(i));
}
std::uint64_t line_of(const std::string& xml, const std::string& token) {
    const auto pos = xml.find(token);
    check(pos != std::string::npos, "missing source token " + token);
    return 1 + static_cast<std::uint64_t>(
                   std::count(xml.begin(), xml.begin() + static_cast<std::ptrdiff_t>(pos), '\n'));
}
bool diagnostic(const ecad::EcadModel& model, const std::string& element, std::uint64_t line) {
    return std::any_of(model.report.unsupported.begin(),
                       model.report.unsupported.end(),
                       [&](const auto& r) { return r.element == element && r.line == line; });
}
auto classified(const ecad::EcadModel& model) {
    std::vector<chipcae::importer::ecad_defects::SkippedRow> skipped;
    return chipcae::importer::ecad_defects::classify_unsupported(model.report, skipped, false);
}
void clean_oval(const ecad::EcadModel& model) {
    for (const auto& r : model.report.unsupported)
        check(r.element != "Oval" && r.element != "StandardPrimitiveRef" &&
                  r.element != "EntryStandard" && r.element != "DictionaryStandard",
              "consumed Oval diagnostic remains: " + r.element);
}
void valid(double w, double h, double degrees, bool after = false) {
    const auto shape =
        "<Oval width=\"" + std::to_string(w) + "\" height=\"" + std::to_string(h) + "\"/>";
    const auto model =
        parse(board(shape, pad("<Xform rotation=\"" + std::to_string(degrees) + "\"/>\n", after)));
    const auto& polygon = single(model);
    check(polygon.points.size() == 258, "Oval must have 258 vertices");
    const double r = 500 * std::min(w, h), a = 500 * std::max(w, h), s = a - r;
    const double expected = 4 * s * r + pi * r * r;
    check(std::abs(area(polygon) - expected) / expected <= epsilon(w * 1000, h * 1000),
          "Oval analytic area bound");
    const double angle = (degrees + (w < h ? 90 : 0)) * pi / 180;
    bounds(polygon,
           10000,
           -20000,
           r + s * std::abs(std::cos(angle)),
           r + s * std::abs(std::sin(angle)));
    // Distance to the analytic centerline also distinguishes +rotation from -rotation.
    for (const auto& p : polygon.points) {
        const double dx = static_cast<double>(p[0]) - 10000, dy = static_cast<double>(p[1]) + 20000;
        check(std::hypot(dx, dy) <= a + 1, "Oval vertex radius bound");
        const double along = dx * std::cos(angle) + dy * std::sin(angle);
        const double across = -dx * std::sin(angle) + dy * std::cos(angle);
        const double distance = std::hypot(std::max(std::abs(along) - s, 0.0), across);
        check(std::abs(distance - r) <= 1, "counterclockwise Pad rotation / single rounding");
    }
    clean_oval(model);
}
void unmodeled(const std::string& shape,
               const std::string& xform,
               bool invalid,
               bool after,
               const std::string& x = "10") {
    const auto xml = board(shape, pad(xform, after, "-20", x));
    const auto model = parse(xml);
    check(layer(model).polygons.empty() && model.vias.empty(), "unsupported Pad created geometry");
    const auto line = line_of(xml, "<StandardPrimitiveRef");
    check(diagnostic(model, "StandardPrimitiveRef", line), "unmodeled SPR must remain");
    check(diagnostic(model, "Oval", line), "missing per-use Oval diagnostic at SPR line");
    check(std::count_if(model.report.unsupported.begin(),
                        model.report.unsupported.end(),
                        [&](const auto& r) { return r.element == "Oval" && r.line == line; }) == 1,
          "per-use Oval diagnostic must be unique");
    if (invalid) {
        check(diagnostic(model, "EntryStandard", line_of(xml, "<EntryStandard")),
              "invalid definition consumed");
        check(diagnostic(model, "Oval", line_of(xml, "<Oval")), "invalid Oval definition lost");
    }
    const auto defects = classified(model);
    const auto id = "board/Oval/" + std::to_string(line);
    check(std::any_of(
              defects.begin(),
              defects.end(),
              [&](const auto& d) { return d.defect_id == id && d.tier == geometry::Tier::C; }),
          "unmodeled Oval must classify C at use");
}
clip::Paths64 paths(const ecad::EcadLayer& item) {
    clip::Paths64 result;
    for (const auto& polygon : item.polygons) {
        clip::Path64 ring;
        for (const auto& p : polygon.points)
            ring.emplace_back(p[0], p[1]);
        result.push_back(std::move(ring));
    }
    return result;
}
// Independent constructive reference: rectangle plus two full 256-gons. The
// production feature is specified as two semicircles; no importer geometry is reused.
clip::Paths64 reference_stadium(std::int64_t cx, std::int64_t cy) {
    clip::Paths64 pieces{{{cx - 1500, cy - 500},
                          {cx + 1500, cy - 500},
                          {cx + 1500, cy + 500},
                          {cx - 1500, cy + 500}}};
    for (const auto offset : {-500, 500}) {
        clip::Path64 disk;
        for (int k = 0; k < 256; ++k) {
            const double t = 2 * pi * k / 256;
            disk.emplace_back(std::llround(static_cast<double>(cx) + 1500 * std::cos(t)),
                              std::llround(static_cast<double>(cy + offset) + 1500 * std::sin(t)));
        }
        pieces.push_back(std::move(disk));
    }
    return clip::Union(pieces, clip::FillRule::NonZero);
}
void fixture_geometry() {
    const auto file = root / "tests/integration/ecad/data/odbpp/kicad4l.ipc2581.xml";
    const auto original = bytes(file);
    for (const auto* id : {"OVAL_1", "OVAL_2"}) {
        const auto begin = original.find("<EntryStandard id=\"" + std::string(id) + "\">");
        check(begin != std::string::npos, "fixture Oval definition missing");
        const auto end = original.find("</EntryStandard>", begin);
        check(original.substr(begin, end - begin).find("<Oval width=\"3.0\" height=\"4.0\"/>") !=
                  std::string::npos,
              "fixture dimensions must independently specify 3 x 4 mm");
    }
    const auto full = read(file); // The acceptance input is the unmodified fixture.
    std::string reference = original;
    std::size_t removed = 0, start = 0;
    std::set<std::string> copper{"F.Cu", "B.Cu", "In1.Cu", "In2.Cu"};
    std::map<std::string, std::set<int>> centers;
    while ((start = reference.find("<LayerFeature layerRef=\"", start)) != std::string::npos) {
        const auto name_start = start + std::string("<LayerFeature layerRef=\"").size();
        const auto name =
            reference.substr(name_start, reference.find('"', name_start) - name_start);
        auto end = reference.find("</LayerFeature>", start);
        if (copper.contains(name)) {
            auto pos = start;
            while ((pos = reference.find("<Pad ", pos)) < end) {
                const auto stop = reference.find("</Pad>", pos) + 6;
                const auto block = reference.substr(pos, stop - pos);
                check(block.find("id=\"OVAL_") != std::string::npos,
                      "reference removes only Oval Pads");
                const int x = block.find("id=\"OVAL_1\"") != std::string::npos ? 17 : 23;
                check(block.find("<Location x=\"" + std::to_string(x) + ".0\" y=\"-22.0\"/>") !=
                          std::string::npos,
                      "fixture Pad center must match independent reference");
                check(centers[name].insert(x).second, "duplicate fixture Pad center");
                reference.erase(pos, stop - pos);
                end -= stop - pos;
                ++removed;
            }
        }
        start = end + 15;
    }
    check(removed == 8, "expected exactly eight copper Oval Pads in fixture");
    for (const auto& name : copper)
        check(centers[name] == std::set<int>{17, 23}, "fixture centers on " + name);
    const auto base = parse(reference);
    check(full.layers.size() == base.layers.size(), "retained layer count changed");
    check(full.outline.points == base.outline.points, "board outline changed");
    check(full.vias.size() == base.vias.size(), "artwork created drilled vias");
    clip::Paths64 pads = reference_stadium(17000, -22000);
    const auto other = reference_stadium(23000, -22000);
    pads.insert(pads.end(), other.begin(), other.end());
    double total_error = 0;
    std::string differing_layers;
    for (std::size_t i = 0; i < full.layers.size(); ++i) {
        const auto& actual = full.layers[i];
        const auto& before = base.layers[i];
        check(actual.name == before.name && actual.material == before.material &&
                  actual.function == before.function && actual.copper == before.copper &&
                  std::abs(actual.thickness_m - before.thickness_m) <= 1e-15,
              "layer attributes changed");
        if (copper.contains(actual.name)) {
            const auto added = clip::Difference(pads, paths(before), clip::FillRule::NonZero);
            const double expected = std::abs(clip::Area(added));
            check(expected > 0, "fixture must expose nonoverlapping Oval copper");
            const double increment = area(actual) - area(before);
            std::cout << "kicad4l " << actual.name << " copper increment=" << increment
                      << " independently expected=" << expected << " DBU^2\n";
            total_error += std::abs(increment - expected);
            auto expected_paths = paths(before);
            // A single source polygon can retain clockwise winding because no Union
            // was needed. Orient the independent copper set positively while retaining
            // the relative winding of any holes produced by an earlier Union.
            if (clip::Area(expected_paths) < 0)
                for (auto& ring : expected_paths)
                    std::reverse(ring.begin(), ring.end());
            expected_paths.insert(expected_paths.end(), pads.begin(), pads.end());
            const auto expected_union = clip::Union(expected_paths, clip::FillRule::NonZero);
            const double xor_area = std::abs(
                clip::Area(clip::Xor(paths(actual), expected_union, clip::FillRule::NonZero)));
            std::cout << "kicad4l " << actual.name << " XOR=" << xor_area
                      << " increment error=" << std::abs(increment - expected) << " DBU^2\n";
            if (xor_area > 2)
                differing_layers += actual.name + " ";
        } else {
            check(actual.polygons.size() == before.polygons.size(),
                  "unaffected polygon count changed");
            for (std::size_t j = 0; j < actual.polygons.size(); ++j)
                check(actual.polygons[j].points == before.polygons[j].points,
                      "unaffected layer geometry changed");
        }
    }
    check(full.omitted_masks.size() == base.omitted_masks.size(), "omitted mask count changed");
    for (std::size_t i = 0; i < full.omitted_masks.size(); ++i) {
        const auto& a = full.omitted_masks[i];
        const auto& b = base.omitted_masks[i];
        check(a.position == b.position && a.layer.name == b.layer.name &&
                  a.layer.material == b.layer.material && a.layer.function == b.layer.function &&
                  a.layer.copper == b.layer.copper && a.layer.metal == b.layer.metal &&
                  std::abs(a.layer.thickness_m - b.layer.thickness_m) <= 1e-15 &&
                  a.layer.polygons.size() == b.layer.polygons.size(),
              "omitted mask changed");
        for (std::size_t j = 0; j < a.layer.polygons.size(); ++j)
            check(a.layer.polygons[j].points == b.layer.polygons[j].points,
                  "mask geometry changed");
    }
    check(differing_layers.empty(),
          differing_layers + ": full copper geometry differs from independent stadium union");
    check(total_error <= 8,
          "copper area increment must match new nonoverlapping area within 8 DBU^2");
}
void fixture_report() {
    const auto model = read(root / "tests/integration/ecad/data/odbpp/kicad4l.ipc2581.xml");
    const auto defects = classified(model);
    std::map<std::string, geometry::Defect> indexed;
    std::array<int, 4> counts{};
    std::vector<std::string> c;
    for (const auto& d : defects) {
        indexed.emplace(d.defect_id, d);
        if (d.tier == geometry::Tier::C)
            c.push_back(d.defect_id);
        const auto name = geometry::tier_name(d.tier);
        const auto i = name == "Weld" ? 0 : name == "A" ? 1 : name == "B" ? 2 : 3;
        ++counts[static_cast<std::size_t>(i)];
    }
    const std::vector<std::pair<std::string, std::vector<int>>> removed{
        {"Oval", {33, 36}},
        {"EntryStandard", {32, 35}},
        {"DictionaryStandard", {31}},
        {"StandardPrimitiveRef", {591, 596, 835, 840, 1079, 1084, 1339, 1344}}};
    for (const auto& [tag, lines] : removed)
        for (const auto line : lines)
            check(!indexed.contains("board/" + tag + "/" + std::to_string(line)),
                  "consumed fixture record remains: " + tag + "/" + std::to_string(line));
    const std::vector<std::pair<std::string, std::vector<int>>> promoted{
        {"StandardPrimitiveRef", {358, 362}},
        {"PadstackPadDef", {278, 282, 286, 290, 297, 301, 305, 309}},
        {"EntryUser", {40, 48}},
        {"DictionaryUser", {39}}};
    for (const auto& [tag, lines] : promoted)
        for (const auto line : lines) {
            const auto id = "board/" + tag + "/" + std::to_string(line);
            check(indexed.contains(id), "definition diagnostic lost: " + id);
            const auto& d = indexed.at(id);
            check(d.tier == geometry::Tier::A &&
                      d.suggestion == "definition expressed by modeled instances",
                  "Oval definition did not become A: " + id);
        }
    check(c == std::vector<std::string>{"board/Oval/1379"}, "only SlotCavity Oval must remain C");
    check(counts == std::array<int, 4>{0, 101, 15, 1}, "fixture (Weld,A,B,C) must be (0,101,15,1)");
    for (const auto line : {605, 610, 849, 854})
        check(indexed.contains("board/StandardPrimitiveRef/" + std::to_string(line)),
              "mask SPR lost");
    for (const bool accept : {false, true}) {
        const auto policy = geometry::apply_repair_policy(defects, {}, accept);
        for (const auto& d : policy.defects)
            if (d.tier == geometry::Tier::A &&
                d.suggestion == "definition expressed by modeled instances")
                check(d.action == d.suggestion && d.rejection_reason.empty(),
                      "definition policy action");
    }
}
} // namespace

int main() {
    int failures = 0;
    const auto run = [&](const std::string& name, const std::function<void()>& test) {
        try {
            test();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& e) {
            ++failures;
            std::cerr << "FAIL " << name << ": " << e.what() << '\n';
        }
    };
    run("vertical_3x4", [] { valid(3, 4, 0); });
    for (const bool after : {false, true}) {
        const auto suffix = after ? "_xform_after_ref" : "_xform_before_ref";
        run("horizontal_rotated_90" + std::string(suffix), [=] {
            valid(4, 3, 90, after);
            const auto a = parse(board());
            const auto b = parse(board("<Oval width=\"4\" height=\"3\"/>",
                                       pad("<Xform rotation=\"90\"/>\n", after)));
            check(std::abs(area(single(a)) - area(single(b))) <= 258,
                  "rotated area difference >258 DBU^2");
        });
        run("noncardinal_rotation_31" + std::string(suffix), [=] { valid(4, 3, 31, after); });
        run("vertical_noncardinal_rotation_minus_31" + std::string(suffix),
            [=] { valid(3, 4, -31, after); });
        for (const auto* transform : {"mirror=\"true\"",
                                      "mirror=\"TRUE\"",
                                      "mirror=\"1\"",
                                      "mirror=\"x\"",
                                      "mirror=\"y\"",
                                      "mirror=\"xy\"",
                                      "mirror=\"\"",
                                      "mirror=\"unknown\"",
                                      "scaleFactor=\"2\"",
                                      "rotation=\"nan\"",
                                      "rotation=\"bad\"",
                                      "scaleFactor=\"inf\"",
                                      "scaleFactor=\"bad\""})
            run(std::string(transform) + suffix, [=] {
                unmodeled("<Oval width=\"3\" height=\"4\"/>",
                          "<Xform " + std::string(transform) + "/>\n",
                          false,
                          after);
            });
        for (const auto* mirror : {"false", "none", "FALSE", "NONE"})
            run(std::string("unmirrored_") + mirror + suffix, [=] {
                const auto model =
                    parse(board("<Oval width=\"3\" height=\"4\"/>",
                                pad("<Xform mirror=\"" + std::string(mirror) + "\"/>\n", after)));
                check(single(model).points.size() == 258, "unmirrored Oval vertex count");
                clean_oval(model);
            });
    }
    for (const auto* dimensions : {"width=\"3\"",
                                   "height=\"4\"",
                                   "width=\"0\" height=\"4\"",
                                   "width=\"3\" height=\"0\"",
                                   "width=\"-3\" height=\"4\"",
                                   "width=\"3\" height=\"-4\"",
                                   "width=\"nan\" height=\"4\"",
                                   "width=\"3\" height=\"inf\""})
        run(std::string("invalid_") + dimensions,
            [=] { unmodeled("<Oval " + std::string(dimensions) + "/>", "", true, false); });
    run("equal_dimensions_circle_limit", [] {
        const auto oval = parse(board("<Oval width=\"3\" height=\"3\"/>"));
        const auto circle = parse(board("<Circle diameter=\"3\"/>"));
        check(std::abs(area(single(oval)) - area(single(circle))) / area(single(circle)) <=
                  2 * epsilon(3000, 3000),
              "equal dimensions must approach Circle area");
        check(single(oval).points == single(circle).points,
              "equal dimensions must match Circle vertices and order");
        clean_oval(oval);
    });
    for (const bool after : {false, true}) {
        run(after ? "rotated_circle_limit_xform_after_ref"
                  : "rotated_circle_limit_xform_before_ref",
            [=] {
                const auto oval = parse(board("<Oval width=\"3\" height=\"3\"/>",
                                              pad("<Xform rotation=\"31\"/>\n", after)));
                const auto circle = parse(board("<Circle diameter=\"3\"/>"));
                check(single(oval).points == single(circle).points,
                      "rotated equal dimensions must match Circle vertices and order");
                clean_oval(oval);
            });
        run(after ? "mirrored_circle_limit_xform_after_ref"
                  : "mirrored_circle_limit_xform_before_ref",
            [=] {
                unmodeled(
                    "<Oval width=\"3\" height=\"3\"/>", "<Xform mirror=\"x\"/>\n", false, after);
            });
    }
    run("subgrid_collapsed_oval_stays_unmodeled",
        [] { unmodeled("<Oval width=\"1e-12\" height=\"2e-12\"/>", "", false, false); });
    run("subgrid_collapsed_circle_limit_stays_unmodeled",
        [] { unmodeled("<Oval width=\"1e-12\" height=\"1e-12\"/>", "", false, false); });
    // The center is within 1024 DBU of Clipper's limit; the 2000 DBU end cap crosses it.
    // Positive x rejects the first cap, negative x the second. Neither may emit a partial ring.
    for (const auto* x : {"2305843009213693", "-2305843009213693"}) {
        run(std::string("out_of_range_oval_stays_unmodeled_x_") + x,
            [=] { unmodeled("<Oval width=\"4\" height=\"3\"/>", "", false, false, x); });
        run(std::string("out_of_range_circle_limit_stays_unmodeled_x_") + x,
            [=] { unmodeled("<Oval width=\"4\" height=\"4\"/>", "", false, false, x); });
    }
    run("overlapping_ovals_union", [] {
        const auto model =
            parse(board("<Oval width=\"3\" height=\"4\"/>", pad() + pad("", false, "-19.5")));
        const double expected = 3000000 + pi * 1500 * 1500 + 1500000;
        check(std::abs(area(single(model)) - expected) / expected <= 2 * epsilon(3000, 4000),
              "union must count only new 0.5 x 3 mm strip once");
    });
    run("mask_no_geometry_or_per_use_oval", [] {
        const auto xml = board("<Oval width=\"3\" height=\"4\"/>", pad(), true);
        const auto model = parse(xml);
        check(layer(model).polygons.empty() && model.vias.empty(), "mask created copper or via");
        check(!diagnostic(model, "Oval", line_of(xml, "<StandardPrimitiveRef")),
              "mask per-use Oval reported");
        check(model.omitted_masks.size() == 1 && model.omitted_masks[0].layer.polygons.empty(),
              "mask artwork modeled");
    });
    run("slot_cavity_stays_C", [] {
        const auto xml = board("<Circle diameter=\"3\"/>",
                               "",
                               false,
                               "<SlotCavity><Oval width=\"2\" height=\"1\"/></SlotCavity>\n");
        const auto defects = classified(parse(xml));
        const auto id = "board/Oval/" + std::to_string(line_of(xml, "<Oval"));
        check(std::any_of(
                  defects.begin(),
                  defects.end(),
                  [&](const auto& d) { return d.defect_id == id && d.tier == geometry::Tier::C; }),
              "SlotCavity Oval must remain C");
    });
    run("circle_exact_baseline", [] {
        const auto xml = board("<Circle diameter=\"3\"/>");
        const auto model = parse(xml);
        const auto& polygon = single(model);
        check(polygon.points.size() == 256, "Circle vertex count changed");
        for (int k = 0; k < 256; ++k) {
            const double angle = 2 * pi * k / 256;
            const Point expected{std::llround(10000 + 1500 * std::cos(angle)),
                                 std::llround(-20000 + 1500 * std::sin(angle))};
            check(polygon.points[static_cast<std::size_t>(k)] == expected,
                  "Circle vertex formula/order changed");
        }
        using Record = std::tuple<std::string, std::string, std::uint64_t, std::string>;
        std::vector<Record> actual;
        for (const auto& r : model.report.unsupported)
            actual.emplace_back(r.code, r.element, r.line, r.context);
        const std::vector<Record> expected{
            {"IMPORT_UNSUPPORTED_ELEMENT", "SolderMask:Mask", 0, ""}};
        check(actual == expected, "Circle exact diagnostic collection changed");
    });
    // Every fixture pad is isolated separately: board-wide Union can merge it into a trace.
    for (const auto* name : {"F.Cu", "B.Cu", "In1.Cu", "In2.Cu"})
        for (const auto x : {17, 23})
            run(std::string("fixture_pad_") + name + "_" + std::to_string(x), [=] {
                auto xml = board("<Oval width=\"3\" height=\"4\"/>",
                                 pad("", false, "-22", std::to_string(x)));
                for (std::size_t pos = 0; (pos = xml.find("\"Cu\"", pos)) != std::string::npos;) {
                    xml.replace(pos, 4, "\"" + std::string(name) + "\"");
                    pos += std::string(name).size() + 2;
                }
                const auto model = parse(xml);
                const auto& polygons = layer(model, name).polygons;
                check(polygons.size() == 1, "missing isolated fixture Oval polygon");
                check(polygons[0].points.size() == 258, "fixture Oval vertex count");
                bounds(polygons[0], x * 1000, -22000, 1500, 2000);
                check(model.vias.empty(), "fixture artwork generated via");
            });
    run("kicad4l_exact_report_delta", fixture_report);
    run("kicad4l_independent_copper_union", fixture_geometry);
    std::cout << failures << " failing cases\n";
    return failures ? 1 : 0;
}
