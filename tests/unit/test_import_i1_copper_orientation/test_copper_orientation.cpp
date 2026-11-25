#include "chipcae/schema/stack.hpp"
#include "copper_guard.hpp"
#include "stack_writer.hpp"

#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <jsoncons/json.hpp>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
namespace fs = std::filesystem;
using J = jsoncons::json;
using chipcae::mesh::CopperSpec;
using chipcae::mesh::Layer;
using chipcae::mesh::StackModel;
const fs::path source{CHIPCAE_SOURCE_DIR};
const auto schema = source / "schema/stack-0.2.schema.json";

struct Checks {
    std::vector<std::string> failures;
    void expect(bool value, const std::string& message) {
        if (!value)
            failures.push_back(message);
    }
    void finish() const {
        if (failures.empty())
            return;
        std::string message;
        for (const auto& failure : failures)
            message += "\n  " + failure;
        throw std::runtime_error(message);
    }
};

J read_json(const fs::path& path) {
    std::ifstream in(path);
    if (!in)
        throw std::runtime_error("cannot read " + path.string());
    return J::parse(in);
}
void write_json(const fs::path& path, const J& j) {
    std::ofstream out(path);
    out << j << '\n';
    if (!out)
        throw std::runtime_error("cannot write " + path.string());
}

struct Fixture {
    fs::path dir;
    J input;
    std::size_t index = 0;
    explicit Fixture(bool polygons) {
        std::random_device random;
        for (int attempt = 0; attempt < 100; ++attempt) {
            dir = fs::temp_directory_path() / ("chipcae-i1-" + std::to_string(random()));
            if (fs::create_directory(dir))
                break;
            if (attempt == 99)
                throw std::runtime_error("cannot create unique temporary directory");
        }
        const auto examples = source / "schema/examples";
        const auto name = polygons ? "copper_polygons.stack.json" : "fcbga_cu.stack.json";
        fs::copy_file(examples / name, dir / "input.json");
        if (polygons)
            fs::copy_file(examples / "copper_polygons.polyset.json",
                          dir / "copper_polygons.polyset.json");
        input = read_json(dir / "input.json");
        while (!input.at("layers").at(index).contains("copper"))
            ++index;
        if (!polygons && input.at("layers").at(index).at("name") != "top_copper")
            throw std::runtime_error("fraction fixture must use top_copper");
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
    ~Fixture() {
        std::error_code error;
        fs::remove_all(dir, error);
    }
    J& layer(J& j) const { return j["layers"][index]; }
    J& copper(J& j) const { return layer(j)["copper"]; }
    auto load(const J& j) const {
        write_json(dir / "input.json", j);
        return chipcae::mesh::load_stack(dir / "input.json", schema);
    }
    std::string path() const { return "$/layers/" + std::to_string(index); }
};

void accepted(Checks& checks, const Fixture& f, const J& j, const std::string& context) {
    auto result = f.load(j);
    checks.expect(static_cast<bool>(result),
                  context +
                      (result ? "" : ": " + result.error().path + " " + result.error().reason));
}
void rejected(Checks& checks,
              const Fixture& f,
              const J& j,
              const std::string& path,
              const std::string& context) {
    auto result = f.load(j);
    checks.expect(!result, context + ": accepted invalid stack");
    if (!result) {
        checks.expect(result.error().path == path,
                      context + ": expected " + path + ", got " + result.error().path);
        checks.expect(!result.error().reason.empty(), context + ": empty error reason");
    }
}

void import_i1_copper_orientation_defaults() {
    Checks checks;
    for (bool polygons : {false, true}) {
        Fixture f(polygons);
        for (bool explicit_defaults : {false, true}) {
            auto j = f.input;
            if (explicit_defaults)
                for (auto& layer : j["layers"].array_range())
                    if (layer.contains("copper")) {
                        layer["copper"]["orientation"] = "planar";
                        layer["copper"]["role"] = "metal";
                    }
            auto model = f.load(j);
            checks.expect(
                static_cast<bool>(model),
                std::string(explicit_defaults ? "explicit defaults" : "original fixture") +
                    (model ? "" : ": " + model.error().reason));
            if (model)
                for (const auto& layer : model.value().layers)
                    if (layer.copper) {
                        checks.expect(layer.copper->orientation == "planar", "default orientation");
                        checks.expect(layer.copper->role == "metal", "default role");
                    }
        }
    }
    checks.finish();
}

void import_i1_copper_orientation_schema() {
    Checks checks;
    // Catch missing schema keys, enum/type checks, combination exclusion and legacy constraints.
    for (bool polygons : {false, true}) {
        Fixture f(polygons);
        const std::string form = polygons ? "polygons " : "fraction ";
        for (const std::string orientation : {"", "planar", "vertical"})
            for (const std::string role : {"", "metal", "opening"}) {
                auto j = f.input;
                if (!orientation.empty())
                    f.copper(j)["orientation"] = orientation;
                if (!role.empty())
                    f.copper(j)["role"] = role;
                const auto context = form + "role=" + role + " orientation=" + orientation;
                if (role == "opening" && orientation == "vertical")
                    rejected(checks, f, j, f.path() + "/copper", context);
                else
                    accepted(checks, f, j, context);
            }
        for (const auto& [key, values] : std::vector<std::pair<std::string, J>>{
                 {"role", J::parse(R"(["Metal","",null,1,["opening"]])")},
                 {"orientation", J::parse(R"(["Vertical","PLANAR","",null,0,["vertical"]])")}})
            for (const auto& value : values.array_range()) {
                auto j = f.input;
                f.copper(j)[key] = value;
                rejected(checks,
                         f,
                         j,
                         f.path() + "/copper/" + key,
                         form + key + "=" + value.to_string());
            }
        for (const std::string key : {"orient", "Role"}) {
            auto j = f.input;
            f.copper(j)[key] = "vertical";
            rejected(checks, f, j, f.path() + "/copper/" + key, form + "unknown " + key);
        }
        auto neither = f.input;
        f.copper(neither).erase("fraction");
        f.copper(neither).erase("polygons");
        rejected(checks, f, neither, f.path() + "/copper", form + "missing alternatives");
        auto both = f.input;
        f.copper(both)["fraction"] = 0.5;
        f.copper(both)["polygons"] = J::parse(R"({"file":"unused.json","layer":"F.Cu"})");
        rejected(checks, f, both, f.path() + "/copper", form + "both alternatives");
        auto no_metal = f.input;
        f.copper(no_metal).erase("metal");
        rejected(checks, f, no_metal, f.path() + "/copper", form + "missing metal");
        for (const std::string variant : {"default", "vertical", "opening"})
            for (const std::string defect : {"kind", "material", "regions"}) {
                auto j = f.input;
                if (variant == "vertical")
                    f.copper(j)["orientation"] = "vertical";
                if (variant == "opening")
                    f.copper(j)["role"] = "opening";
                if (defect == "kind")
                    f.layer(j)["kind"] = "Explicit";
                else {
                    // A valid region satisfies the existing material-or-regions schema rule,
                    // so the loader's base-material and no-regions checks are reached.
                    J region;
                    region["name"] = "synthetic_region";
                    region["material"] = "synthetic_region_material";
                    region["footprint"] = j.at("outline");
                    f.layer(j)["regions"] = J::array();
                    f.layer(j)["regions"].push_back(region);
                    if (defect == "material")
                        f.layer(j).erase("material");
                }
                const auto suffix = defect == "kind" ? "copper" : defect;
                rejected(checks, f, j, f.path() + "/" + suffix, form + variant + " " + defect);
            }
    }
    // A compiled schema may be reused only while its complete contents match.
    // Keep rereading caller-supplied schemas, including edits at the same path.
    Fixture f(false);
    auto vertical = f.input;
    f.copper(vertical)["orientation"] = "vertical";
    write_json(f.dir / "input.json", vertical);
    const auto alternate = f.dir / "schema.json";
    auto specification = read_json(schema);
    write_json(alternate, specification);
    checks.expect(static_cast<bool>(chipcae::mesh::load_stack(f.dir / "input.json", alternate)),
                  "identical schema at another path rejected vertical");
    specification["$defs"]["copper"]["properties"]["orientation"]["enum"] =
        J::parse(R"(["planar"])");
    write_json(alternate, specification);
    const auto restricted = chipcae::mesh::load_stack(f.dir / "input.json", alternate);
    checks.expect(!restricted, "edited caller schema was ignored");
    if (!restricted) {
        checks.expect(restricted.error().path == f.path() + "/copper/orientation",
                      "edited schema error path changed");
        checks.expect(!restricted.error().reason.empty(), "edited schema error reason empty");
    }
    write_json(alternate, read_json(schema));
    checks.expect(static_cast<bool>(chipcae::mesh::load_stack(f.dir / "input.json", alternate)),
                  "restored schema rejected vertical");
    checks.finish();
}

void compare_layers(Checks& checks, const StackModel& before, const StackModel& after) {
    checks.expect(before.layers.size() == after.layers.size(), "layer count changed");
    for (std::size_t i = 0; i < before.layers.size() && i < after.layers.size(); ++i) {
        const auto& a = before.layers[i];
        const auto& b = after.layers[i];
        checks.expect(a.material == b.material, a.name + " base material changed");
        checks.expect(a.copper.has_value() == b.copper.has_value(), a.name + " copper lost");
        if (!a.copper || !b.copper)
            continue;
        checks.expect(a.copper->orientation == b.copper->orientation, a.name + " orientation lost");
        checks.expect(a.copper->role == b.copper->role,
                      a.name + " role lost (opening became metal)");
        checks.expect(a.copper->metal == b.copper->metal, a.name + " fill material changed");
        // Exact parsed numeric equality is the frozen serialization contract, not a solver
        // tolerance.
        checks.expect(a.copper->fraction == b.copper->fraction, a.name + " fraction changed");
        checks.expect(a.copper->polygons.has_value() == b.copper->polygons.has_value(),
                      a.name + " polygons lost");
        if (a.copper->polygons && b.copper->polygons) {
            checks.expect(a.copper->polygons->file == b.copper->polygons->file,
                          a.name + " polygon file changed");
            checks.expect(a.copper->polygons->layer == b.copper->polygons->layer,
                          a.name + " polygon layer changed");
        }
    }
}

void roundtrip(Checks& checks, const Fixture& f, const J& input, bool legacy) {
    auto loaded = f.load(input);
    checks.expect(static_cast<bool>(loaded),
                  "roundtrip load" + (loaded ? std::string{} : ": " + loaded.error().reason));
    if (!loaded)
        return;
    const auto out = f.dir / "output.json";
    const auto written = chipcae::importer::write_stack(loaded.value(), out);
    checks.expect(static_cast<bool>(written), "write_stack failed");
    if (!written)
        return;
    const auto output = read_json(out);
    if (legacy) {
        // The revised brief permits only the baseline loader's body defaults.
        // Build the complete expected document from the input, never the writer output.
        auto expected = input;
        for (auto& layer : expected["layers"].array_range()) {
            if (!layer.contains("body"))
                layer["body"] = layer.at("name");
            if (layer.contains("regions"))
                for (auto& region : layer["regions"].array_range())
                    if (!region.contains("body"))
                        region["body"] = region.at("name");
        }
        checks.expect(output == expected, "legacy document changed beyond baseline body defaults");
    }
    if (input.contains("external_refs"))
        checks.expect(output.at("external_refs") == input.at("external_refs"),
                      "sidecar hash changed");
    for (std::size_t i = 0; i < input.at("layers").size(); ++i) {
        const auto& layer = input.at("layers").at(i);
        if (!layer.contains("copper"))
            continue;
        auto expected = layer.at("copper");
        if (expected.contains("orientation") && expected.at("orientation") == "planar")
            expected.erase("orientation");
        if (expected.contains("role") && expected.at("role") == "metal")
            expected.erase("role");
        checks.expect(output.at("layers").at(i).at("copper") == expected,
                      "copper JSON keys/values changed in layer " + std::to_string(i));
    }
    auto reloaded = chipcae::mesh::load_stack(out, schema);
    checks.expect(static_cast<bool>(reloaded), "cannot reload writer output");
    if (reloaded)
        compare_layers(checks, loaded.value(), reloaded.value());
}

void import_i1_copper_orientation_roundtrip() {
    Checks checks;
    for (bool polygons : {false, true}) {
        Fixture f(polygons);
        roundtrip(checks, f, f.input, true);
        for (const auto& [orientation, role] : std::vector<std::pair<std::string, std::string>>{
                 {"planar", "metal"}, {"vertical", "metal"}, {"planar", "opening"}}) {
            auto j = f.input;
            f.copper(j)["orientation"] = orientation;
            f.copper(j)["role"] = role;
            roundtrip(checks, f, j, false);
        }
        auto multiple = f.input;
        auto opening = f.layer(multiple);
        opening["name"] = "separate_opening";
        opening["copper"]["role"] = "opening";
        f.copper(multiple)["orientation"] = "vertical";
        multiple["layers"].push_back(opening);
        roundtrip(checks, f, multiple, false);
    }
    checks.finish();
}

void import_i1_copper_orientation_opening_base() {
    Checks checks;
    for (bool polygons : {false, true}) {
        Fixture f(polygons);
        auto j = f.input;
        f.layer(j)["material"] = "synthetic_mask_base";
        f.copper(j)["metal"] = "synthetic_opening_fill";
        f.copper(j)["role"] = "opening";
        auto loaded = f.load(j);
        checks.expect(static_cast<bool>(loaded), "opening stack rejected");
        if (loaded) {
            const auto& layer = loaded.value().layers.at(f.index);
            checks.expect(layer.material == "synthetic_mask_base",
                          "opening replaced base material");
            checks.expect(layer.copper && layer.copper->metal == "synthetic_opening_fill",
                          "opening fill changed");
            checks.expect(layer.copper && layer.material != layer.copper->metal,
                          "base and fill must differ");
        }
        roundtrip(checks, f, j, false);
    }
    checks.finish();
}

void import_i1_copper_orientation_guard() {
    Checks checks;
    Layer layer;
    layer.name = "synthetic_guard_layer";
    try {
        chipcae::solve::detail::require_planar_metal_copper(layer);
        layer.copper = CopperSpec{};
        chipcae::solve::detail::require_planar_metal_copper(layer);
    } catch (const std::exception& error) {
        checks.expect(false, std::string("default copper threw: ") + error.what());
    }
    for (const auto& [orientation, role] : std::vector<std::pair<std::string, std::string>>{
             {"vertical", "metal"}, {"planar", "opening"}, {"vertical", "opening"}}) {
        layer.copper = CopperSpec{};
        layer.copper->orientation = orientation;
        layer.copper->role = role;
        const auto field =
            role == "opening" ? "copper role opening" : "copper orientation vertical";
        try {
            chipcae::solve::detail::require_planar_metal_copper(layer);
            checks.expect(false, std::string(field) + " did not throw INPUT_INVALID(14)");
        } catch (const chipcae::solve::InputError& error) {
            checks.expect(error.code == chipcae::ErrorCode::INPUT_INVALID && error.code == 14,
                          "guard wrong exit code");
            const std::string message = error.what();
            checks.expect(message.find(layer.name) != std::string::npos,
                          "guard missing layer name");
            checks.expect(message.find(field) != std::string::npos,
                          "guard missing " + std::string(field));
            checks.expect(message.find("not supported until solve-via-vertical-mixing") !=
                              std::string::npos,
                          "guard missing required suffix");
            if (role == "opening" && orientation == "vertical")
                checks.expect(message.find("copper orientation") == std::string::npos ||
                                  message.find("copper role") < message.find("copper orientation"),
                              "role must take precedence over orientation");
        } catch (const std::exception& error) {
            checks.expect(false, std::string("wrong guard exception: ") + error.what());
        }
    }
    checks.finish();
}
} // namespace

int main() {
    int failures = 0;
    for (const auto& [name, test] : std::vector<std::pair<std::string, std::function<void()>>>{
             {"import_i1_copper_orientation_defaults", import_i1_copper_orientation_defaults},
             {"import_i1_copper_orientation_schema", import_i1_copper_orientation_schema},
             {"import_i1_copper_orientation_roundtrip", import_i1_copper_orientation_roundtrip},
             {"import_i1_copper_orientation_opening_base",
              import_i1_copper_orientation_opening_base},
             {"import_i1_copper_orientation_guard", import_i1_copper_orientation_guard}}) {
        try {
            test();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    }
    return failures == 0 ? 0 : 1;
}
