#include "chipcae/geometry/ecad/ipc2581.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <utility>

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

static bool lookup_acceptance() {
    using namespace chipcae::geometry::ecad;
    const ScopedMap temporary;
    const auto path = temporary.path();
    int failures = 0;
    auto run = [&](const char* name,
                   std::string source,
                   bool copper,
                   const char* json,
                   bool expected,
                   const std::string& value) {
        EcadModel m;
        m.layers = {{"Core", source, "", 0, copper, {}}};
        {
            std::ofstream out(path);
            out << json;
        }
        std::string error;
        const bool ok = apply_material_map(m, path, &error);
        const bool pass =
            ok == expected &&
            (ok ? (copper ? m.layers[0].material == "base_eq" && m.layers[0].metal == value
                          : m.layers[0].material == value)
                : error == value);
        std::cout << (pass ? "PASS " : "FAIL ") << name << ": " << error << '\n';
        if (!pass)
            ++failures;
    };
    run("exact precedes case and layer",
        "SiO2",
        false,
        R"({"SiO2":"exact","sio2":"lower","SIO2":"upper","layer:Core":"layer"})",
        true,
        "exact");
    run("unique ASCII case fallback",
        "SiO2",
        false,
        R"({"sio2":"case","layer:Core":"layer"})",
        true,
        "case");
    run("case ambiguity sorted",
        "SiO2",
        false,
        R"({"sio2":"a","SIO2":"b"})",
        false,
        "MATERIAL_MAPPING_MISSING: 1 layer(s) unmapped: Core (tried: \"SiO2\", \"layer:Core\"; "
        "case-conflict: \"SIO2\", \"sio2\")");
    run("ambiguous case can fall through to layer",
        "SiO2",
        false,
        R"({"sio2":"a","SIO2":"b","layer:Core":"layer"})",
        true,
        "layer");
    run("layer key empty source", "", false, R"({"layer:Core":"layer"})", true, "layer");
    run("layer precedes copper",
        "missing",
        true,
        R"({"layer:Core":"layer","copper":"cu","copper_base":"base_eq"})",
        true,
        "layer");
    run("copper empty source", "", true, R"({"copper":"cu","copper_base":"base_eq"})", true, "cu");
    run("copper unmapped source",
        "COPPER",
        true,
        R"({"copper":"cu","copper_base":"base_eq"})",
        true,
        "cu");
    run("noncopper never defaults",
        "",
        false,
        R"({"copper":"cu"})",
        false,
        "MATERIAL_MAPPING_MISSING: 1 layer(s) unmapped: Core (tried: \"layer:Core\")");
    run("layer names exact",
        "",
        false,
        R"({"layer:core":"a"})",
        false,
        "MATERIAL_MAPPING_MISSING: 1 layer(s) unmapped: Core (tried: \"layer:Core\")");
    run("duplicate exact keys invalid",
        "x",
        false,
        R"({"x":"a","x":"b"})",
        false,
        "invalid material map: duplicate material name");
    run("differently cased keys legal", "x", false, R"({"X":"a","x":"b"})", true, "b");
    EcadModel m;
    m.layers = {{"A", "SiO2", "", 0, false, {}},
                {"B", "", "", 0, false, {}},
                {"C", "metal", "", 0, true, {}}};
    {
        std::ofstream out(path);
        out << "{}";
    }
    std::string error;
    bool ok = apply_material_map(m, path, &error);
    bool pass = !ok && error.find("MATERIAL_MAPPING_MISSING: 3 layer(s) unmapped:") == 0;
    for (auto entry : {"A (tried: \"SiO2\", \"layer:A\")",
                       "B (tried: \"layer:B\")",
                       "C (tried: \"metal\", \"layer:C\", \"copper\")"})
        pass = pass && error.find(entry) != std::string::npos;
    std::cout << (pass ? "PASS " : "FAIL ") << "empty map aggregates all attempted keys: " << error
              << '\n';
    if (!pass)
        ++failures;
    std::filesystem::remove(path);
    return failures == 0;
}

// These cases catch metal being reused as the base, one-pass neighbour lookup,
// reversed tie-breaking, and reserved keys leaking into ordinary material lookup.
static bool copper_base_acceptance() {
    using namespace chipcae::geometry::ecad;
    const ScopedMap temporary;
    int failures = 0;
    auto check = [&](bool pass, const std::string& name, const std::string& detail) {
        std::cout << (pass ? "PASS " : "FAIL ") << name << ": " << detail << '\n';
        failures += pass ? 0 : 1;
    };
    auto run = [&](const char* name,
                   std::vector<EcadLayer> layers,
                   const char* json,
                   std::size_t index,
                   const char* base,
                   const char* metal = "cu") {
        EcadModel model;
        model.layers = std::move(layers);
        {
            std::ofstream out(temporary.path());
            out << json;
        }
        std::string error;
        const bool ok = apply_material_map(model, temporary.path(), &error);
        const auto& layer = model.layers[index];
        check(ok && layer.material == base && layer.metal == metal,
              name,
              "base=" + layer.material + ", metal=" + layer.metal + ", " + error);
    };
    const EcadLayer copper{"F.Cu", "Copper", "SIGNAL", 35e-6, true, {}};
    const EcadLayer upper{"Upper", "FR4_A", "", 1e-3, false, {}};
    const EcadLayer lower{"Lower", "FR4_B", "", 1e-3, false, {}};
    const char* neighbours =
        R"({"copper":"cu","FR4_A":"upper_eq","FR4_B":"lower_eq","copper_base":"global_eq"})";
    run("layer override beats neighbour and global",
        {upper, copper, lower},
        R"({"copper":"cu","FR4_A":"upper_eq","FR4_B":"lower_eq","copper_base":"global_eq","copper_base@F.Cu":"local_eq"})",
        1,
        "local_eq");
    run("later neighbour uses mapped id (two passes)", {copper, lower}, neighbours, 0, "lower_eq");
    run("earlier neighbour beats global", {upper, copper}, neighbours, 1, "upper_eq");
    run("equidistant neighbour chooses larger index",
        {upper, copper, lower},
        neighbours,
        1,
        "lower_eq");
    EcadLayer other = copper;
    other.name = "Inner.Cu";
    run("nearer upper neighbour beats farther lower",
        {upper, copper, other, lower},
        neighbours,
        1,
        "upper_eq");
    run("nearer lower neighbour beats farther upper",
        {upper, other, copper, lower},
        neighbours,
        2,
        "lower_eq");
    run("no dielectric uses global", {copper}, neighbours, 0, "global_eq");
    run("duplicate reserved global key uses last value",
        {copper},
        R"({"copper":"cu","copper_base":"old","copper_base":"global_eq"})",
        0,
        "global_eq");
    run("duplicate reserved layer key uses last value",
        {copper, lower},
        R"({"copper":"cu","FR4_B":"lower_eq","copper_base":"global_eq","copper_base@F.Cu":"old","copper_base@F.Cu":"local_eq"})",
        0,
        "local_eq");
    run("exact metal precedes case, layer and copper",
        {copper},
        R"({"Copper":"gold","COPPER":"wrong","layer:F.Cu":"wrong","copper":"wrong","copper_base":"base_eq"})",
        0,
        "base_eq",
        "gold");
    run("unique case metal precedes layer and copper",
        {copper},
        R"({"COPPER":"case_metal","layer:F.Cu":"wrong","copper_base":"base_eq"})",
        0,
        "base_eq",
        "case_metal");
    auto empty_source = copper;
    empty_source.material.clear();
    run("empty source uses layer metal before copper",
        {empty_source},
        R"({"layer:F.Cu":"layer_metal","copper":"wrong","copper_base":"base_eq"})",
        0,
        "base_eq",
        "layer_metal");

    run("override name is case sensitive",
        {copper},
        R"({"copper":"cu","copper_base@f.cu":"wrong","copper_base":"global_eq"})",
        0,
        "global_eq");
    run("explicit same base and metal id is legal",
        {copper},
        R"({"copper":"cu","copper_base":"cu"})",
        0,
        "cu");
    auto reject = [&](const char* name,
                      std::vector<EcadLayer> layers,
                      const char* json,
                      const std::string& diagnostic) {
        EcadModel model;
        model.layers = std::move(layers);
        {
            std::ofstream out(temporary.path());
            out << json;
        }
        std::string error;
        const bool ok = apply_material_map(model, temporary.path(), &error);
        check(!ok && error == diagnostic, name, error);
    };
    reject("metal mapped but base absent",
           {copper},
           R"({"copper":"cu"})",
           "MATERIAL_MAPPING_MISSING: 1 layer(s) unmapped: F.Cu (tried: \"copper_base@F.Cu\", "
           "\"copper_base\")");
    reject("base present but metal absent",
           {copper},
           R"({"copper_base":"base_eq"})",
           "MATERIAL_MAPPING_MISSING: 1 layer(s) unmapped: F.Cu (tried: \"Copper\", "
           "\"layer:F.Cu\", \"copper\")");
    reject("global reserved key is case sensitive",
           {copper},
           R"({"copper":"cu","Copper_Base":"wrong"})",
           "MATERIAL_MAPPING_MISSING: 1 layer(s) unmapped: F.Cu (tried: \"copper_base@F.Cu\", "
           "\"copper_base\")");
    reject("missing bases count each layer once in layer order",
           {copper, other},
           R"({"copper":"cu"})",
           "MATERIAL_MAPPING_MISSING: 2 layer(s) unmapped: F.Cu (tried: \"copper_base@F.Cu\", "
           "\"copper_base\"); Inner.Cu (tried: \"copper_base@Inner.Cu\", \"copper_base\")");
    for (const auto& source :
         {"copper_base", "COPPER_BASE", "copper_base@F.Cu", "COPPER_BASE@F.CU"}) {
        auto reserved = copper;
        reserved.material = source;
        run("reserved material name cannot supply metal",
            {reserved},
            R"({"copper":"cu","copper_base":"base_eq","copper_base@F.Cu":"local_eq"})",
            0,
            "local_eq");
        reserved.copper = false;
        reject("reserved material name cannot supply dielectric",
               {reserved},
               R"({"copper_base":"base_eq","copper_base@F.Cu":"local_eq"})",
               "MATERIAL_MAPPING_MISSING: 1 layer(s) unmapped: F.Cu (tried: \"" +
                   std::string(source) + "\", \"layer:F.Cu\")");
    }
    // An unmapped dielectric must still fail overall, never becoming a usable source id.
    EcadModel invalid;
    invalid.layers = {copper, lower};
    {
        std::ofstream out(temporary.path());
        out << R"({"copper":"cu","copper_base":"global_eq"})";
    }
    std::string error;
    const bool ok = apply_material_map(invalid, temporary.path(), &error);
    check(!ok && error.find("MATERIAL_MAPPING_MISSING:") == 0 &&
              error.find("Lower") != std::string::npos && invalid.layers[0].material != "FR4_B",
          "unmapped neighbour cannot supply source material as base",
          error);

    invalid.layers = {copper, lower};
    {
        std::ofstream out(temporary.path());
        out << R"({"copper":"cu"})";
    }
    const bool no_base = apply_material_map(invalid, temporary.path(), &error);
    check(!no_base && error.find("MATERIAL_MAPPING_MISSING: 2 layer(s) unmapped:") == 0 &&
              error.find("copper_base@F.Cu") != std::string::npos &&
              error.find("Lower") != std::string::npos &&
              error.find("\"copper_base\"") != std::string::npos &&
              error.find("F.Cu") < error.rfind("Lower"),
          "failed nearest dielectric is diagnosed with attempted base keys in layer order",
          error);
    std::filesystem::remove(temporary.path());
    error.clear();
    check(!apply_material_map(invalid, temporary.path(), &error) &&
              error == "MATERIAL_MAPPING_MISSING: cannot open material map",
          "missing mapping file unchanged",
          error);
    return failures == 0;
}

static std::string read_text(const std::filesystem::path& file) {
    std::ifstream input(file);
    return {(std::istreambuf_iterator<char>(input)), {}};
}

static bool output_acceptance() {
    using namespace chipcae::geometry::ecad;
    const ScopedMap temporary;
    int failures = 0;
    auto check = [&](bool pass, const char* name) {
        std::cout << (pass ? "PASS " : "FAIL ") << name << '\n';
        failures += pass ? 0 : 1;
    };
    const auto stack = temporary.directory / "stack.json";
    const auto sidecar = temporary.directory / "stack.polyset";
    // Independent fields isolate exporter bugs from mapper bugs.
    EcadModel model;
    model.outline.points = {{0, 0}, {4000, 0}, {4000, 3000}, {0, 3000}};
    model.layers = {{"F.Cu",
                     "base_eq",
                     "SIGNAL",
                     35e-6,
                     true,
                     {{{{100, 100}, {1100, 100}, {1100, 1100}, {100, 1100}}}},
                     "cu"}};
    std::string error;
    const bool written = to_stack(model, stack, sidecar, &error);
    const auto text = read_text(stack);
    check(
        written && text.find(R"("material":"base_eq")") != std::string::npos &&
            text.find(
                R"("copper":{"metal":"cu","polygons":{"file":"stack.polyset","layer":"F.Cu"}})") !=
                std::string::npos &&
            text.find("fraction") == std::string::npos,
        "export keeps base and metal independent and copper keys exactly metal/polygons");
    model.layers[0].material.clear();
    check(!to_stack(model, stack, sidecar, &error), "export rejects missing base");
    model.layers[0].material = "base_eq";
    model.layers[0].metal.clear();
    check(!to_stack(model, stack, sidecar, &error), "export rejects missing metal");
    model.layers[0].material = "cu";
    model.layers[0].metal = "cu";
    check(to_stack(model, stack, sidecar, &error), "export accepts explicitly identical ids");

    const auto root = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto fixture = root / "tests/integration/ecad/data/board_4layer.xml";
    const auto map = root / "tests/integration/import_cli/material_map.json";
    check(std::filesystem::is_regular_file(fixture) && std::filesystem::is_regular_file(map),
          "repository fixture and real CLI map exist");
    auto imported = read_ipc2581(fixture);
    check(static_cast<bool>(imported), "board_4layer parses");
    if (imported) {
        check(apply_material_map(imported.model, map, &error), "board_4layer maps");
        std::size_t copper_count = 0;
        for (const auto& layer : imported.model.layers) {
            if (!layer.copper)
                continue;
            ++copper_count;
            check(layer.material == "fr4_eq" && layer.metal == "cu",
                  "board_4layer copper has fr4_eq base and cu metal");
        }
        check(copper_count == 4, "board_4layer covers all four copper layers");
        check(to_stack(imported.model, stack, sidecar, &error), "board_4layer exports");
        const auto json = read_text(stack);
        std::size_t position = 0, objects = 0;
        bool valid = json.find("fraction") == std::string::npos;
        while ((position = json.find(R"("copper":{)", position)) != std::string::npos) {
            const auto end = json.find("}}", position);
            const auto object = json.substr(position, end - position + 2);
            valid = valid && object.find(R"("metal":"cu")") != std::string::npos &&
                    object.find(R"("polygons":{)") != std::string::npos;
            ++objects;
            position += 10;
        }
        check(valid && objects == 4,
              "board_4layer exports four polygon copper objects without fraction");
    }
    return failures == 0;
}

int main() {
    using namespace chipcae::geometry::ecad;
    EcadModel model;
    model.layers = {{"core", "FR4", "", 0.0, false, {}}, {"top", "", "SIGNAL", 0.0, true, {}}};
    const auto path = std::filesystem::temp_directory_path() / "chipcae-material-map-test.json";
    {
        std::ofstream out(path);
        out << R"({"FR4":"mat.fr4","copper":"mat.cu"})";
    }
    std::string error;
    bool smoke = true;
    if (!apply_material_map(model, path, &error) || model.layers[0].material != "mat.fr4" ||
        model.layers[1].material != "mat.fr4" || model.layers[1].metal != "mat.cu") {
        std::cerr << "valid map was not applied: " << error << '\n';
        smoke = false;
    }
    {
        std::ofstream out(path);
        out << R"({"FR4":"mat.fr4"})";
    }
    model.layers[1].material.clear();
    if (apply_material_map(model, path, &error) ||
        error.find("MATERIAL_MAPPING_MISSING") == std::string::npos) {
        std::cerr << "missing copper mapping was not rejected: " << error << '\n';
        smoke = false;
    }
    std::filesystem::remove(path);
    const bool lookup = lookup_acceptance();
    const bool copper = copper_base_acceptance();
    const bool output = output_acceptance();
    return smoke && lookup && copper && output ? 0 : 2;
}
