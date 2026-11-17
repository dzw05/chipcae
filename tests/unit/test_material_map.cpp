#include "chipcae/geometry/ecad/ipc2581.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>

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
        const bool pass = ok == expected && (ok ? m.layers[0].material == value : error == value);
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
        R"({"layer:Core":"layer","copper":"cu"})",
        true,
        "layer");
    run("copper empty source", "", true, R"({"copper":"cu"})", true, "cu");
    run("copper unmapped source", "COPPER", true, R"({"copper":"cu"})", true, "cu");
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
    if (!apply_material_map(model, path, &error) || model.layers[0].material != "mat.fr4" ||
        model.layers[1].material != "mat.cu") {
        std::cerr << "valid map was not applied: " << error << '\n';
        return 1;
    }
    {
        std::ofstream out(path);
        out << R"({"FR4":"mat.fr4"})";
    }
    model.layers[1].material.clear();
    if (apply_material_map(model, path, &error) ||
        error.find("MATERIAL_MAPPING_MISSING") == std::string::npos) {
        std::cerr << "missing copper mapping was not rejected: " << error << '\n';
        return 1;
    }
    std::filesystem::remove(path);
    return lookup_acceptance() ? 0 : 2;
}
