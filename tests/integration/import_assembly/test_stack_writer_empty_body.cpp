#include "writer_test_support.hpp"

#include <chrono>

int main() {
    using namespace chipcae::mesh;
    using sp4_test::require;
    try {
#ifdef SP4_HAS_JSONCONS
        const auto dir =
            std::filesystem::temp_directory_path() /
            ("sp4-writer-empty-body-" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(dir);
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::filesystem::remove_all(path); }
        } cleanup{dir};

        StackModel model;
        model.id = "empty-body";
        model.symmetry = "none";
        model.package_type = "test";
        model.outline.box = {0, 0, .01, .005};
        Layer layer;
        layer.name = "cavity";
        layer.kind = "Explicit";
        layer.thickness = .001;
        layer.regions = {{"die", "silicon", "", {{.001, .001, .002, .002}, {}}}};
        model.layers.push_back(layer);

        const auto output = dir / "empty.json";
        const auto written = chipcae::importer::write_stack(model, output);
        require(bool(written), written ? "" : written.error().reason);
        const auto first = sp4_test::bytes(output);
        const auto json = jsoncons::json::parse(first);
        require(!json.at("layers").at(0).contains("body"), "omit empty layer body");
        require(!json.at("layers").at(0).at("regions").at(0).contains("body"),
                "omit empty region body");

        // load_stack validates against stack-0.2 before applying the name fallback.
        const auto loaded = load_stack(
            output, std::filesystem::path(CHIPCAE_SOURCE_DIR) / "schema/stack-0.2.schema.json");
        require(bool(loaded), loaded ? "" : loaded.error().reason);
        require(loaded.value().layers.size() == 1, "preserve Explicit layer");
        const auto& restored = loaded.value().layers.at(0);
        require(restored.kind == "Explicit", "preserve layer kind");
        require(restored.body == "cavity", "missing layer body defaults to name");
        require(restored.regions.size() == 1, "preserve Explicit region");
        require(restored.regions.at(0).body == "die", "missing region body defaults to name");

        require(bool(chipcae::importer::write_stack(loaded.value(), output)), "write loaded stack");
        const auto second = sp4_test::bytes(output);
        const auto second_json = jsoncons::json::parse(second);
        require(second_json.at("layers").at(0).at("body").as<std::string>() == "cavity",
                "write nonempty layer body even when equal to name");
        require(second_json.at("layers").at(0).at("regions").at(0).at("body").as<std::string>() ==
                    "die",
                "write nonempty region body even when equal to name");

        auto normalized = json;
        for (auto& entry : normalized.at("layers").array_range()) {
            if (!entry.contains("body"))
                entry["body"] = entry.at("name").as<std::string>();
            if (entry.contains("regions")) {
                for (auto& region : entry.at("regions").array_range()) {
                    if (!region.contains("body"))
                        region["body"] = region.at("name").as<std::string>();
                }
            }
        }
        require(normalized == second_json,
                "first and second writes must be field-by-field equal after body defaults");

        const auto reloaded = load_stack(
            output, std::filesystem::path(CHIPCAE_SOURCE_DIR) / "schema/stack-0.2.schema.json");
        require(bool(reloaded), reloaded ? "" : reloaded.error().reason);
        require(bool(chipcae::importer::write_stack(reloaded.value(), output)),
                "write reloaded stack");
        require(second == sp4_test::bytes(output),
                "second and third writes must be byte-identical");
#else
        require(false, "empty body writer test requires jsoncons headers");
#endif
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
