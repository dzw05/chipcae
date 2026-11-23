#include "chipcae/geometry/ecad/ipc2581.hpp"
#ifdef CHIPCAE_SOLDERMASK_STACK_VALIDATION
#include "chipcae/schema/stack.hpp"
#endif

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace {
using namespace chipcae::geometry::ecad;
const std::filesystem::path root = CHIPCAE_SOURCE_DIR;
const auto data = root / "tests/integration/ecad/data";

struct Temporary {
    std::filesystem::path path;
    Temporary() {
        std::random_device random;
        do {
            path = std::filesystem::temp_directory_path() /
                   ("soldermask-" + std::to_string(random()) + "-" + std::to_string(random()));
        } while (!std::filesystem::create_directory(path));
    }
    ~Temporary() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
    std::filesystem::path write(const std::string& name, const std::string& text) const {
        const auto file = path / name;
        std::ofstream out(file);
        out << text;
        if (!out)
            throw std::runtime_error("cannot write test input " + name);
        return file;
    }
};
void require(bool value, const std::string& message) {
    if (!value)
        throw std::runtime_error(message);
}
EcadModel read(const std::filesystem::path& file) {
    auto result = read_ipc2581(file);
    require(bool(result), "read_ipc2581: " + result.error);
    return std::move(result.model);
}
std::vector<std::string> names(const EcadModel& model) {
    std::vector<std::string> result;
    for (const auto& layer : model.layers)
        result.push_back(layer.name);
    return result;
}
const EcadLayer& layer(const EcadModel& model, const std::string& name) {
    const auto found = std::find_if(
        model.layers.begin(), model.layers.end(), [&](const auto& l) { return l.name == name; });
    require(found != model.layers.end(), "missing layer " + name);
    return *found;
}
bool same_number(double a, double b) {
    return (std::isnan(a) && std::isnan(b)) ||
           (std::isinf(a) && std::isinf(b) && std::signbit(a) == std::signbit(b)) ||
           (std::isfinite(a) && std::isfinite(b) && std::abs(a - b) <= 1e-15);
}
bool same_layers(const EcadModel& a, const EcadModel& b) {
    if (a.layers.size() != b.layers.size())
        return false;
    for (std::size_t i = 0; i < a.layers.size(); ++i) {
        const auto& x = a.layers[i];
        const auto& y = b.layers[i];
        if (std::tie(x.name, x.material, x.function, x.copper, x.metal) !=
                std::tie(y.name, y.material, y.function, y.copper, y.metal) ||
            !same_number(x.thickness_m, y.thickness_m) || x.polygons.size() != y.polygons.size())
            return false;
        for (std::size_t j = 0; j < x.polygons.size(); ++j)
            if (x.polygons[j].points != y.polygons[j].points)
                return false;
    }
    return true;
}
using Record = std::tuple<std::string, std::string, std::uint64_t>;
std::vector<Record> records(const EcadModel& model, const std::vector<std::string>& removed = {}) {
    std::vector<Record> result;
    for (const auto& entry : model.report.unsupported)
        if (std::find(removed.begin(), removed.end(), entry.element) == removed.end())
            result.emplace_back(entry.code, entry.element, entry.line);
    return result;
}
void unchanged(const EcadModel& model, const EcadModel& before) {
    require(same_layers(model, before), "existing layers changed");
    require(records(model) == records(before), "existing unsupported records changed");
}
void restore(EcadModel& model, const std::filesystem::path& map) {
    std::string error;
    require(restore_solder_masks(model, map, &error), "restore_solder_masks: " + error);
}
void map_layers(EcadModel& model, const std::filesystem::path& map) {
    std::string error;
    require(apply_material_map(model, map, &error), "apply_material_map: " + error);
}
const std::string base_map = R"("FR4":"core_eq","copper":"cu")";
std::string mapping(const std::string& extra) {
    return "{" + base_map + (extra.empty() ? "" : "," + extra) + "}";
}

// Five layers isolate the nearest-base tie rule from the public fixture's extra layers.
std::string board(const std::string& mask_attributes = "thickness=\"0.010\"",
                  bool mask_stack = true,
                  const std::string& material = "SOLDERMASK",
                  const std::string& explicit_material = "") {
    std::string xml =
        R"(<IPC-2581><Ecad><CadHeader units="MILLIMETER"><Spec name="mask"><General type="MATERIAL"><Property/><Property text=")" +
        material + R"("/><Property text="ignored"/></General></Spec></CadHeader><CadData>)";
    for (const auto& [name, function] :
         std::vector<std::pair<std::string, std::string>>{{"F.Mask", "soldermask"},
                                                          {"F.Cu", "CONDUCTOR"},
                                                          {"Core", "DIELCORE"},
                                                          {"B.Cu", "CONDUCTOR"},
                                                          {"B.Mask", "SOLDERMASK"}})
        xml += "<Layer name=\"" + name + "\" layerFunction=\"" + function + "\"/>";
    xml += "<Stackup><StackupGroup>";
    for (const auto& name : {"F.Mask", "F.Cu", "Core", "B.Cu", "B.Mask"}) {
        const std::string n = name;
        const bool mask = n.ends_with("Mask");
        if (mask && !mask_stack)
            continue;
        xml += "<StackupLayer layerOrGroupRef=\"" + n + "\" ";
        xml += mask ? mask_attributes + " " + explicit_material
                    : (n == "Core" ? "thickness=\"1.6\" matDes=\"FR4\"" : "thickness=\"0.035\"");
        xml += mask ? "><SpecRef id=\"mask\"/></StackupLayer>" : "/>";
    }
    return xml +
           R"(</StackupGroup></Stackup><Step name="board"><Profile><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="10" y="0"/><PolyStepSegment x="10" y="5"/><PolyStepSegment x="0" y="5"/></Polygon></Profile></Step></CadData></Ecad></IPC-2581>)";
}
void check_mask(const EcadLayer& mask, const std::string& material) {
    require(!mask.copper, mask.name + " must be non-copper");
    require(mask.polygons.empty(), mask.name + " must have empty polygons");
    require(mask.material == material, mask.name + " material expected " + material);
    require(std::abs(mask.thickness_m / 1e-5 - 1) <= 1e-12, mask.name + " thickness");
}
} // namespace

int main() {
    int failures = 0;
    auto test = [&](const std::string& name, const std::function<void()>& run) {
        try {
            run();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& e) {
            ++failures;
            std::cerr << "FAIL " << name << ": " << e.what() << '\n';
        }
    };
    test("duplicate B.Mask capture and layer-key restoration without SpecRef", [] {
        const Temporary tmp;
        auto model = read(data / "kicad_like/soldermask_b.xml");
        require(model.omitted_masks.size() == 1, "expected exactly one captured B.Mask");
        const auto& mask = model.omitted_masks.front();
        require(mask.layer.name == "B.Mask" && mask.position == model.layers.size(),
                "B.Mask capture position");
        require(mask.layer.material.empty(), "mask without SpecRef has no source material");
        const auto map =
            tmp.write("map.json", R"({"layer:B.Mask":"mask_eq","layer:DIELECTRIC_1":"core_eq"})");
        restore(model, map);
        require(names(model) == std::vector<std::string>{"DIELECTRIC_1", "B.Mask"},
                "duplicate StackupLayer must restore exactly once");
        require(records(model).empty(), "restored B.Mask diagnostic must be removed");
        check_mask(layer(model, "B.Mask"), "");
        map_layers(model, map);
        check_mask(layer(model, "B.Mask"), "mask_eq");
    });
    test("duplicate fixture without mask mapping is unchanged", [] {
        const Temporary tmp;
        auto model = read(data / "kicad_like/soldermask_b.xml");
        const auto before = model;
        restore(model, tmp.write("map.json", R"({"layer:DIELECTRIC_1":"core_eq"})"));
        unchanged(model, before);
    });
    test("public fixture source capture, restoration, mapping, stack-0.2 validation", [] {
        const Temporary tmp;
        auto model = read(data / "odbpp/kicad4l.ipc2581.xml");
        const auto before = model;
        const auto original = names(before);
        require(model.omitted_masks.size() == 2, "expected F.Mask and B.Mask captures");
        require(model.omitted_masks[0].position == 0 &&
                    model.omitted_masks[1].position == original.size(),
                "mask positions count retained layers");
        for (const auto& mask : model.omitted_masks)
            check_mask(mask.layer, "SOLDERMASK");
        const auto map = tmp.write("map.json", mapping(R"("SOLDERMASK":"mask_eq")"));
        restore(model, map);
        auto expected = original;
        expected.insert(expected.begin(), "F.Mask");
        expected.push_back("B.Mask");
        require(names(model) == expected, "restored masks must enclose the original layer order");
        auto filtered = model;
        std::erase_if(filtered.layers, [](const auto& l) { return l.name.ends_with(".Mask"); });
        require(same_layers(filtered, before), "non-mask layers must remain identical");
        require(records(model) == records(before, {"SolderMask:F.Mask", "SolderMask:B.Mask"}),
                "only exact mask records may disappear, retaining code/element/line/order");
        for (const auto& name : {"F.Mask", "B.Mask"})
            check_mask(layer(model, name), "SOLDERMASK");
        restore(model, map);
        require(names(model) == expected, "repeat restoration must not duplicate masks");
        map_layers(model, map);
        for (const auto& name : {"F.Mask", "B.Mask"})
            check_mask(layer(model, name), "mask_eq");
        require(layer(model, "F.Cu").material == layer(model, "DIELECTRIC_1").material,
                "F.Cu base must choose mapped DIELECTRIC_1 on equal distance");
        require(layer(model, "B.Cu").material == "mask_eq", "B.Cu base must choose B.Mask");
        std::string error;
        const auto stack = tmp.path / "stack.json";
        require(to_stack(model, stack, tmp.path / "board.polyset", &error), "to_stack: " + error);
#ifdef CHIPCAE_SOLDERMASK_STACK_VALIDATION
        auto loaded = chipcae::mesh::load_stack(stack, root / "schema/stack-0.2.schema.json");
        require(bool(loaded), loaded ? "" : "stack-0.2: " + loaded.error().reason);
        require(loaded.value().layers.size() == expected.size(), "exported layer count");
        for (const auto i : {std::size_t{0}, expected.size() - 1}) {
            const auto& mask = loaded.value().layers[i];
            require(mask.name == expected[i] && mask.kind == "Homogenized" &&
                        mask.material == "mask_eq",
                    "exported masks must be Homogenized mask_eq");
            require(std::abs(mask.thickness / 1e-5 - 1) <= 1e-12, "exported mask thickness");
        }
#else
        require(false, "stack-0.2 validation requires CHIPCAE_ENABLE_JSONCONS=ON");
#endif
    });
    test("one layer key restores only F.Mask and removes only its record", [] {
        const Temporary tmp;
        auto model = read(data / "odbpp/kicad4l.ipc2581.xml");
        const auto before = model;
        auto expected = names(before);
        expected.insert(expected.begin(), "F.Mask");
        restore(model, tmp.write("map.json", mapping(R"("layer:F.Mask":"mask_eq")")));
        require(names(model) == expected, "layer:F.Mask must restore only F.Mask");
        require(records(model) == records(before, {"SolderMask:F.Mask"}),
                "B.Mask record and every other record must remain");
    });
    test("synthetic Core wins F.Cu base tie; B.Mask wins B.Cu tie", [] {
        const Temporary tmp;
        auto model = read(tmp.write("board.xml", board()));
        const auto map = tmp.write("map.json", mapping(R"("SOLDERMASK":"mask_eq")"));
        restore(model, map);
        require(names(model) ==
                    std::vector<std::string>{"F.Mask", "F.Cu", "Core", "B.Cu", "B.Mask"},
                "five-layer synthetic order");
        map_layers(model, map);
        require(layer(model, "F.Cu").material == "core_eq", "F.Cu must choose Core");
        require(layer(model, "B.Cu").material == "mask_eq", "B.Cu must choose B.Mask");
    });

    struct Lookup {
        std::string name, source, keys, expected;
    };
    for (const auto& c : std::vector<Lookup>{
             {"exact before folded and layer",
              "SOLDERmask",
              R"("SOLDERmask":"exact_eq","soldermask":"fold_eq","layer:F.Mask":"layer_eq")",
              "exact_eq"},
             {"unique folded before layer",
              "SOLDERmask",
              R"("soldermask":"fold_eq","layer:F.Mask":"layer_eq")",
              "fold_eq"},
             {"ambiguous folded without layer",
              "SOLDERmask",
              R"("soldermask":"lower_eq","SOLDERMASK":"upper_eq")",
              ""},
             {"ambiguous folded falls through to exact layer",
              "SOLDERmask",
              R"("soldermask":"lower_eq","SOLDERMASK":"upper_eq","layer:F.Mask":"layer_eq")",
              "layer_eq"},
             {"layer fallback", "SOLDERMASK", R"("layer:F.Mask":"layer_eq")", "layer_eq"},
             {"layer keys are case sensitive", "SOLDERMASK", R"("layer:f.mask":"mask_eq")", ""},
             {"no copper fallback", "SOLDERMASK", "", ""},
             {"reserved keys never restore", "copper_base", R"("copper_base":"base_eq")", ""}}) {
        test("lookup: " + c.name, [&] {
            const Temporary tmp;
            auto model = read(tmp.write("board.xml", board("thickness=\"0.010\"", true, c.source)));
            const auto before = model;
            const auto map = tmp.write("map.json", mapping(c.keys));
            restore(model, map);
            if (c.expected.empty()) {
                unchanged(model, before);
            } else {
                require(names(model).front() == "F.Mask", "lookup must restore F.Mask");
                require(layer(model, "F.Mask").material == c.source,
                        "restoration must retain source material until mapping");
                map_layers(model, map);
                require(layer(model, "F.Mask").material == c.expected, "lookup precedence");
            }
        });
    }
    for (const auto& text : {std::string("{}"), mapping("")})
        test("unmapped masks leave public fixture unchanged: " + text, [&] {
            const Temporary tmp;
            auto model = read(data / "odbpp/kicad4l.ipc2581.xml");
            const auto before = model;
            restore(model, tmp.write("map.json", text));
            unchanged(model, before);
        });
    for (const auto& attribute :
         {"thickness=\"0\"", "thickness=\"-0.010\"", "", "thickness=\"nan\"", "thickness=\"inf\""})
        test(std::string("ineligible original thickness: ") + attribute, [&] {
            const Temporary tmp;
            auto model = read(tmp.write("board.xml", board(attribute)));
            const auto before = model;
            restore(model, tmp.write("map.json", mapping(R"("SOLDERMASK":"mask_eq")")));
            unchanged(model, before);
        });
    test("missing mask StackupLayer leaves layers and report unchanged", [] {
        const Temporary tmp;
        auto model = read(tmp.write("board.xml", board("", false)));
        const auto before = model;
        restore(model, tmp.write("map.json", mapping(R"("SOLDERMASK":"mask_eq")")));
        unchanged(model, before);
    });
    for (const auto& explicit_value : {"matDes=\"EXPLICIT\"", "matDes=\"\""})
        test(std::string("explicit matDes beats SpecRef: ") + explicit_value, [&] {
            const Temporary tmp;
            auto model = read(tmp.write(
                "board.xml", board("thickness=\"0.010\"", true, "SOLDERMASK", explicit_value)));
            require(model.omitted_masks.size() == 2, "explicit-material masks must be captured");
            const std::string expected =
                std::string(explicit_value) == "matDes=\"\"" ? "" : "EXPLICIT";
            for (const auto& mask : model.omitted_masks)
                require(mask.layer.material == expected, "explicit source material must win");
            const auto before = model;
            restore(model, tmp.write("map.json", mapping(R"("SOLDERMASK":"mask_eq")")));
            unchanged(model, before);
        });
    test("invalid target restored and mapped before normal validation rejects", [] {
        const Temporary tmp;
        auto model = read(tmp.write("board.xml", board()));
        const auto map = tmp.write("map.json", mapping(R"("SOLDERMASK":"mask")"));
        restore(model, map);
        require(names(model).front() == "F.Mask", "target validation must not gate restoration");
        map_layers(model, map);
        std::string error;
        require(!validate_mapped_targets(model, &error), "mask target without _eq must fail");
        require(error == "layer 'F.Mask' material 'mask': homogenized material must end with _eq",
                "normal target error text: " + error);
    });
    for (const auto& text : {"not JSON",
                             "[]",
                             "{\"x\":1}",
                             "{\"x\":\"a\",\"x\":\"b\"}",
                             "{\"x\":\"a\"} trailing",
                             "{\"x\":\"\"}",
                             "{",
                             "missing"})
        test(std::string("map parsing error parity and no mutation: ") + text, [&] {
            const Temporary tmp;
            auto model = read(tmp.write("board.xml", board()));
            const auto before = model;
            const auto file = std::string(text) == "missing" ? tmp.path / "missing.json"
                                                             : tmp.write("map.json", text);
            std::string expected, actual;
            auto mapped = model;
            require(!apply_material_map(mapped, file, &expected), "invalid map must fail mapping");
            require(!restore_solder_masks(model, file, &actual),
                    "invalid map must fail restoration");
            require(actual == expected, "map parser diagnostic must match: " + actual);
            unchanged(model, before);
        });
    test("stable equal-position merge, partial selection, and exact report filtering", [] {
        const Temporary tmp;
        EcadModel model;
        model.layers = {{"Core", "FR4", "DIELCORE", 1e-3, false, {}}};
        for (const auto& name : {"First", "Ineligible", "Second"}) {
            EcadLayer mask{
                name, "SOLDERMASK", "SOLDERMASK", 1e-5, true, {{{{0, 0}, {1, 0}, {0, 1}}}}};
            if (mask.name == "Ineligible")
                mask.thickness_m = std::numeric_limits<double>::quiet_NaN();
            model.omitted_masks.push_back({mask, 0});
        }
        model.omitted_masks.push_back({{"Last", "SOLDERMASK", "SOLDERMASK", 1e-5, false, {}}, 1});
        model.report.unsupported = {{"before", "metadata", 1},
                                    {"IMPORT_UNSUPPORTED_ELEMENT", "SolderMask:First", 2},
                                    {"keep", "SolderMask:First:artwork", 3},
                                    {"IMPORT_UNSUPPORTED_ELEMENT", "SolderMask:Ineligible", 4},
                                    {"IMPORT_UNSUPPORTED_ELEMENT", "SolderMask:Second", 5},
                                    {"IMPORT_UNSUPPORTED_ELEMENT", "SolderMask:Last", 6},
                                    {"after", "metadata", 7}};
        const auto before = model;
        restore(model, tmp.write("map.json", mapping(R"("SOLDERMASK":"mask_eq")")));
        require(names(model) == std::vector<std::string>{"First", "Second", "Core", "Last"},
                "merge counts only selected masks, preserving equal-position capture order");
        for (const auto& name : {"First", "Second", "Last"})
            check_mask(layer(model, name), "SOLDERMASK");
        require(records(model) ==
                    records(before, {"SolderMask:First", "SolderMask:Second", "SolderMask:Last"}),
                "stable filter must preserve ineligible and nonexact report entries");
    });
    test("partial restoration preserves original slots and existing via references", [] {
        const Temporary tmp;
        EcadModel model;
        model.layers = {{"F.Cu", "", "CONDUCTOR", 35e-6, true, {}},
                        {"Core", "FR4", "DIELCORE", 1e-3, false, {}},
                        {"B.Cu", "", "CONDUCTOR", 35e-6, true, {}}};
        for (const auto& name : {"First", "Second", "Last"})
            model.omitted_masks.push_back({{name, "SOLDERMASK", "SOLDERMASK", 1e-5, false, {}},
                                           std::string(name) == "Last" ? 3u : 0u});
        Via via;
        via.id = "through";
        via.start_layer = "F.Cu";
        via.end_layer = "B.Cu";
        via.position = {12, 34};
        via.outer_radius_dbu = 20;
        via.inner_radius_dbu = 15;
        via.dielectric_layers = {1};
        model.vias.push_back(via);
        ViaFraction cell;
        cell.layer = 1;
        cell.lower = {0, 0};
        cell.upper = {40, 40};
        cell.fraction = 0.25;
        cell.via_count = 1;
        model.via_fractions.push_back(cell);
        restore(model, tmp.write("first.json", mapping(R"("layer:Second":"mask_eq")")));
        require(names(model) == std::vector<std::string>{"Second", "F.Cu", "Core", "B.Cu"},
                "partial equal-position restoration");
        require(model.vias.front().dielectric_layers == std::vector<std::size_t>{2} &&
                    model.via_fractions.front().layer == 2,
                "via indices must track Core after partial restoration");
        const auto full = tmp.write("full.json", mapping(R"("SOLDERMASK":"mask_eq")"));
        restore(model, full);
        const std::vector<std::string> expected{"First", "Second", "F.Cu", "Core", "B.Cu", "Last"};
        require(names(model) == expected, "later restoration must use original capture slots");
        restore(model, full);
        require(names(model) == expected, "repeat restoration must preserve order and count");
        const auto& after = model.vias.front();
        require(after.dielectric_layers == std::vector<std::size_t>{3} &&
                    model.via_fractions.front().layer == 3,
                "via indices must still identify Core after later and repeat restoration");
        require(after.id == via.id && after.start_layer == via.start_layer &&
                    after.end_layer == via.end_layer && after.position == via.position &&
                    same_number(after.outer_radius_dbu, via.outer_radius_dbu) &&
                    same_number(after.inner_radius_dbu, via.inner_radius_dbu),
                "restoration must preserve via geometry");
        const auto& fraction = model.via_fractions.front();
        require(same_number(fraction.lower[0], cell.lower[0]) &&
                    same_number(fraction.lower[1], cell.lower[1]) &&
                    same_number(fraction.upper[0], cell.upper[0]) &&
                    same_number(fraction.upper[1], cell.upper[1]) &&
                    same_number(fraction.fraction, cell.fraction) &&
                    fraction.via_count == cell.via_count,
                "restoration must preserve existing via fractions and cell bounds");
    });
    std::cout << failures << " acceptance case(s) failed\n";
    return failures == 0 ? 0 : 1;
}
