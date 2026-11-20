#include "chipcae/geometry/ecad/ipc2581.hpp"
#if CHIPCAE_ECAD_MAP_TARGETS_HAS_STACK
#include "chipcae/schema/stack.hpp"
#endif

#include <bit>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
namespace fs = std::filesystem;
using namespace chipcae::geometry::ecad;
const fs::path source = CHIPCAE_SOURCE_DIR;
constexpr const char* id_rule = "must match ^[a-z0-9_-]+$ (lowercase letters, digits, '_' or '-'; "
                                "no dots, e.g. cu, fr4_eq)";
struct Acceptance {
    int failures = 0;

    void check(bool condition, const std::string& message) {
        if (!condition) {
            ++failures;
            std::cerr << "ecad-map-targets: " << message << '\n';
        }
    }
    static void require(bool condition, const std::string& message) {
        if (!condition)
            throw std::runtime_error(message);
    }
    struct TemporaryDirectory {
        fs::path path;
        TemporaryDirectory() {
            auto pattern = (fs::temp_directory_path() / "chipcae-ecad-map-targets-XXXXXX").string();
            require(::mkdtemp(pattern.data()) != nullptr, "cannot create temporary directory");
            path = pattern;
        }
        ~TemporaryDirectory() {
            std::error_code ignored;
            fs::remove_all(path, ignored);
        }
    };
    void write(const fs::path& path, const std::string& bytes) {
        std::ofstream out(path, std::ios::binary);
        out << bytes;
        out.close();
        require(static_cast<bool>(out), "cannot write test input " + path.filename().string());
    }
    std::string read(const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        require(static_cast<bool>(in), "cannot read " + path.filename().string());
        return {std::istreambuf_iterator<char>(in), {}};
    }
    EcadLayer dielectric(std::string name, std::string material) {
        EcadLayer layer;
        layer.name = std::move(name);
        layer.material = std::move(material);
        layer.function = "DIELECTRIC";
        layer.thickness_m = 1e-3;
        return layer;
    }
    EcadLayer copper(std::string material, std::string metal) {
        auto layer = dielectric("Copper", std::move(material));
        layer.copper = true;
        layer.function = "SIGNAL";
        layer.metal = std::move(metal);
        layer.polygons.push_back({{{0, 0}, {100, 0}, {100, 100}, {0, 100}}});
        return layer;
    }
    EcadModel model_of(std::vector<EcadLayer> layers) {
        EcadModel model;
        model.layers = std::move(layers);
        model.outline.points = {{0, 0}, {1000, 0}, {1000, 1000}, {0, 1000}};
        return model;
    }
    std::string id_error(const std::string& name, const std::string& field, const std::string& id) {
        return "layer '" + name + "' " + field + " '" + id + "': " + id_rule;
    }
    std::string suffix_error(const std::string& name, const std::string& id) {
        return "layer '" + name + "' material '" + id + "': homogenized material must end with _eq";
    }
    void mapped(EcadModel& model, const fs::path& map, const std::string& bytes) {
        write(map, bytes);
        std::string error;
        require(apply_material_map(model, map, &error), "apply must succeed: " + error);
    }
    // Independent snapshot of every populated field in the synthetic models. Floating values
    // are compared by representation because validation must leave the model untouched.
    std::string snapshot(const EcadModel& model) {
        std::ostringstream out;
        out << model.id << '\n' << std::bit_cast<std::uint64_t>(model.dbu_m) << '\n';
        for (const auto& p : model.outline.points)
            out << p[0] << ',' << p[1] << '\n';
        for (const auto& layer : model.layers) {
            out << layer.name << '\n'
                << layer.material << '\n'
                << layer.metal << '\n'
                << layer.function << '\n'
                << layer.copper << '\n'
                << std::bit_cast<std::uint64_t>(layer.thickness_m) << '\n';
            for (const auto& polygon : layer.polygons) {
                out << "polygon\n";
                for (const auto& p : polygon.points)
                    out << p[0] << ',' << p[1] << '\n';
            }
        }
        return out.str();
    }
    void rejects(const EcadModel& model, const fs::path& dir, const std::string& expected) {
        const auto before = snapshot(model);
        std::string first, second;
        check(!validate_mapped_targets(model, &first), "validator accepted " + expected);
        check(first == expected,
              "validator diagnostic: expected [" + expected + "], got [" + first + "]");
        check(!validate_mapped_targets(model, &second), "second validation accepted " + expected);
        check(second == first, "repeated validation changed diagnostic");
        check(!validate_mapped_targets(model, nullptr),
              "null-error validation accepted " + expected);
        check(snapshot(model) == before, "validation mutated model fields");

        const auto stack = dir / "fresh.stack.json", polyset = dir / "fresh.polyset";
        std::string error;
        check(!to_stack(model, stack, polyset, &error), "export accepted " + expected);
        check(error == expected,
              "export diagnostic: expected [" + expected + "], got [" + error + "]");
        check(!fs::exists(stack) && !fs::exists(polyset),
              "rejected export published fresh artifacts");

        const auto old_stack = dir / "existing.stack.json", old_polyset = dir / "existing.polyset";
        const std::string stack_bytes("previous stack\0\n", 16),
            polyset_bytes("previous polyset\0\n", 18);
        write(old_stack, stack_bytes);
        write(old_polyset, polyset_bytes);
        error.clear();
        check(!to_stack(model, old_stack, old_polyset, &error),
              "export replaced existing artifacts");
        check(error == expected, "existing-destination diagnostic differs from validator");
        check(read(old_stack) == stack_bytes, "rejected export changed stack sentinel bytes");
        check(read(old_polyset) == polyset_bytes, "rejected export changed polyset sentinel bytes");
    }
    void accepts(const EcadModel& model, const fs::path& dir) {
        std::string error;
        check(validate_mapped_targets(model, &error), "valid target rejected: " + error);
        check(validate_mapped_targets(model, nullptr), "valid target rejected with null error");
        const auto stack = dir / "valid.stack.json", polyset = dir / "valid.polyset";
        check(to_stack(model, stack, polyset, &error), "valid export rejected: " + error);
        check(fs::is_regular_file(stack) && fs::is_regular_file(polyset),
              "valid export missing files");
    }
    template <class F>
    void run(const std::string& name, F fn) {
        const int before = failures;
        try {
            TemporaryDirectory dir;
            fn(dir.path);
        } catch (const std::exception& e) {
            check(false, name + ": " + e.what());
        }
        std::cout << "ecad-map-targets: " << (before == failures ? "PASS " : "FAIL ") << name
                  << '\n';
    }

#if CHIPCAE_ECAD_MAP_TARGETS_HAS_STACK
    void compare_loader(const fs::path& dir, const std::string& material) {
        // Handwritten schema-valid JSON; deliberately independent of the exporter.
        const auto input = dir / "handwritten.json";
        write(input,
              R"({"id":"comparison","unit":"m","outline":{"box":[0,0,0.001,0.001]},"layers":[)"
              R"({"name":"Core","thickness":0.001,"kind":"Homogenized","n_elem_min":1,)"
              R"("grading":1,"material":")" +
                  material + R"("}],"symmetry":"none","package_type":"ipc2581"})");
        const auto loaded =
            chipcae::mesh::load_stack(input, source / "schema/stack-0.2.schema.json");
        const auto model = model_of({dielectric("Core", material)});
        if (material == "fr4_eq") {
            check(static_cast<bool>(loaded), "loader rejected handwritten fr4_eq");
            accepts(model, dir);
        } else if (material == "FR4_eq") {
            check(static_cast<bool>(loaded), "loader must accept uppercase FR4_eq ending in _eq");
            rejects(model, dir, id_error("Core", "material", "FR4_eq"));
        } else {
            check(!loaded, "loader accepted handwritten " + material);
            if (!loaded)
                check(loaded.error().reason == "homogenized material must end with _eq",
                      "loader failed for unrelated reason: " + loaded.error().reason);
            rejects(model, dir, suffix_error("Core", material));
        }
    }
#endif

    int execute() {
        run("official map and board_4layer", [this](const auto& dir) {
            auto result = read_ipc2581(source / "tests/integration/ecad/data/board_4layer.xml");
            require(static_cast<bool>(result), "fixture parse failed: " + result.error);
            std::string error;
            require(apply_material_map(result.model,
                                       source / "tests/integration/import_cli/material_map.json",
                                       &error),
                    "official map failed: " + error);
            std::size_t copper_count = 0;
            for (const auto& layer : result.model.layers) {
                if (layer.copper) {
                    ++copper_count;
                    check(layer.material == "fr4_eq" && layer.metal == "cu", "official copper IDs");
                }
            }
            check(copper_count == 4, "official fixture must contain exactly four copper layers");
            accepts(result.model, dir);
        });
        for (const auto& target : {"mat.bt", "bt", "FR4_eq", "Fr4_eq", "fr4_EQ", "fr4_eq"}) {
            run(std::string("mapped dielectric ") + target, [this, target](const auto& dir) {
                auto model = model_of({dielectric("Core", "BT")});
                mapped(model, dir / "map.json", std::string(R"({"BT":")") + target + R"("})");
                check(model.layers[0].material == target, "apply changed mapping target");
                if (std::string(target) == "fr4_eq")
                    accepts(model, dir);
                else
                    rejects(model,
                            dir,
                            std::string(target) == "bt" ? suffix_error("Core", "bt")
                                                        : id_error("Core", "material", target));
            });
        }
        run("empty dielectric has no export fallback", [this](const auto& dir) {
            rejects(model_of({dielectric("Empty", "")}), dir, id_error("Empty", "material", ""));
        });
        for (const auto& field : {"material", "metal"}) {
            run(std::string("invalid copper ") + field, [this, field](const auto& dir) {
                const bool base = std::string(field) == "material";
                rejects(model_of({copper(base ? "mat.cu" : "fr4_eq", base ? "cu" : "mat.cu")}),
                        dir,
                        id_error("Copper", field, "mat.cu"));
            });
        }
        for (const auto& base : {"cu", "fr4_eq"}) {
            run(std::string("legal copper base ") + base,
                [this, base](const auto& dir) { accepts(model_of({copper(base, "cu")}), dir); });
        }
        run("global copper_base retains illegal target until validation", [this](const auto& dir) {
            auto model = model_of({copper("", "")});
            mapped(model, dir / "map.json", R"({"copper":"cu","copper_base":"mat.bt"})");
            check(model.layers[0].material == "mat.bt" && model.layers[0].metal == "cu",
                  "global copper_base must be applied unchanged");
            rejects(model, dir, id_error("Copper", "material", "mat.bt"));
        });
        for (const auto& target : {"mat.bt", "fr4_eq"}) {
            run(std::string("two source keys share ") + target, [this, target](const auto& dir) {
                auto model = model_of({dielectric("First", "A"), dielectric("Second", "B")});
                mapped(model,
                       dir / "map.json",
                       std::string(R"({"A":")") + target + R"(","B":")" + target + R"("})");
                check(model.layers[0].material == target && model.layers[1].material == target,
                      "both source keys must map");
                if (std::string(target) == "fr4_eq")
                    accepts(model, dir);
                else
                    rejects(model, dir, id_error("First", "material", "mat.bt"));
            });
        }
        run("all IDs precede suffix checks", [this](const auto& dir) {
            rejects(model_of({dielectric("Low", "bt"), dielectric("High", "mat.bt")}),
                    dir,
                    id_error("High", "material", "mat.bt"));
        });
        run("copper material precedes metal", [this](const auto& dir) {
            rejects(model_of({copper("mat.bt", "mat.cu")}),
                    dir,
                    id_error("Copper", "material", "mat.bt"));
        });
        run("lowest index wins ID phase", [this](const auto& dir) {
            rejects(model_of({dielectric("First", "mat.bt"), copper("mat.cu", "mat.cu")}),
                    dir,
                    id_error("First", "material", "mat.bt"));
        });
        run("lowest index wins suffix phase", [this](const auto& dir) {
            rejects(model_of({dielectric("First", "bt"), dielectric("Second", "fr4")}),
                    dir,
                    suffix_error("First", "bt"));
        });
        run("unused mapping entries do not invalidate targets", [this](const auto& dir) {
            auto model = model_of({dielectric("Core", "FR4")});
            mapped(model, dir / "map.json", R"({"FR4":"fr4_eq","unused":"mat.wrong"})");
            fs::remove(dir /
                       "map.json"); // Validation must use the model alone, without reading a map.
            accepts(model, dir);
        });
        run("nonempty copper fields are checked even when the other is missing",
            [this](const auto&) {
                for (const auto& layer : {copper("", "mat.cu"), copper("mat.bt", "")}) {
                    const auto model = model_of({layer});
                    std::string error;
                    check(!validate_mapped_targets(model, &error),
                          "missing copper field hid an invalid nonempty field");
                    check(error == (layer.material.empty()
                                        ? id_error("Copper", "metal", "mat.cu")
                                        : id_error("Copper", "material", "mat.bt")),
                          "invalid nonempty copper field diagnostic: " + error);
                }
            });
        run("empty model and missing copper fields", [this](const auto& dir) {
            std::string error;
            check(validate_mapped_targets(EcadModel{}, &error), "empty model rejected");
            check(validate_mapped_targets(EcadModel{}, nullptr),
                  "empty model with null error rejected");
            for (const auto& layer : {copper("", "cu"), copper("fr4_eq", ""), copper("", "")}) {
                const auto model = model_of({layer});
                check(validate_mapped_targets(model, nullptr),
                      "validator must leave missing copper IDs");
                error.clear();
                check(!to_stack(model, dir / "missing.json", dir / "missing.polyset", &error),
                      "export must still reject missing copper fields");
                check(error ==
                          "MATERIAL_MAPPING_MISSING: copper layer requires base and metal: Copper",
                      "missing copper diagnostic changed");
                check(!fs::exists(dir / "missing.json") && !fs::exists(dir / "missing.polyset"),
                      "missing copper fields published artifacts");
            }
        });
#if CHIPCAE_ECAD_MAP_TARGETS_HAS_STACK
        for (const auto& material : {"fr4_eq", "fr4", "bt", "FR4_eq"})
            run(std::string("load_stack compare ") + material,
                [this, material](const auto& dir) { compare_loader(dir, material); });
#else
        std::cout << "ecad-map-targets: load_stack compare skipped (no chipcae_stack)\n";
#endif
        return failures == 0 ? 0 : 1;
    }
};
} // namespace

int main() {
    return Acceptance{}.execute();
}
