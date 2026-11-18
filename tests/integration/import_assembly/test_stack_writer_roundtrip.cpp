#include "writer_test_support.hpp"

#include <chrono>

int main() {
    using namespace chipcae::mesh;
    using sp4_test::require;
    try {
        const auto dir =
            std::filesystem::temp_directory_path() /
            ("sp4-writer-" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(dir);
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::filesystem::remove_all(path); }
        } cleanup{dir};
#ifdef SP4_HAS_JSONCONS
        Polyset poly{1e-6,
                     {{"F.Cu", {0, 4}, {0, 0, 1000, 0, 1000, 1000, 0, 1000}},
                      {"B.Cu", {0, 4}, {0, 0, 2000, 0, 2000, 1000, 0, 1000}}}};
        require(bool(write_polyset(dir / "copper.polyset", poly)), "write fixture sidecar");
#endif
        StackModel model;
        model.id = "escape\"\\\n雪";
        model.symmetry = "none";
        model.package_type = "test";
        model.outline.box = {0, 0, .01, .005};
        model.outline.polygon = {{0, 0}, {.01, 0}, {.01, .005}, {0, .005}};
        for (const auto& name : {"F.Cu", "B.Cu"}) {
            Layer layer;
            layer.name = name;
            layer.body = "board";
            layer.kind = "Patterned";
            layer.material = "fr4";
            layer.thickness = .000035;
            layer.grading = 1.125;
            layer.n_elem_min = 3;
            layer.copper = CopperSpec{"copper", {}, CopperPolygons{"copper.polyset", name}};
            model.layers.push_back(layer);
        }
        Layer cavity;
        cavity.name = "cavity";
        cavity.body = "assembly";
        cavity.kind = "Explicit";
        cavity.thickness = .000123456789012345;
        cavity.regions = {
            {"box", "silicon", "die", {{.001, .001, .002, .002}, {}}},
            {"polygon",
             "copper",
             "lid",
             {{.003, .001, .004, .002}, {{.003, .001}, {.004, .001}, {.0035, .002}}}}};
        model.layers.push_back(cavity);
        Layer fraction = model.layers[0];
        fraction.name = "fraction";
        fraction.copper = CopperSpec{"copper", .123456789012345, {}};
        model.layers.push_back(fraction);
        model.activation_groups = {
            {"all", {"layer:F.Cu", "layer:B.Cu", "region:box", "body:lid", "interface:F.Cu|B.Cu"}}};
        const auto sidecar_before = sp4_test::bytes(dir / "copper.polyset");
        const auto output = dir / "out.json";
        const auto result = chipcae::importer::write_stack(model, output);
        require(bool(result),
                "write_stack must serialize a complete StackModel" +
                    (result ? std::string{} : ": " + result.error().reason));
#ifdef SP4_HAS_JSONCONS
        auto loaded = load_stack(
            output, std::filesystem::path(CHIPCAE_SOURCE_DIR) / "schema/stack-0.2.schema.json");
        require(bool(loaded), loaded ? "" : loaded.error().reason);
        sp4_test::equal(model, loaded.value());
        require(sidecar_before == sp4_test::bytes(dir / "copper.polyset"),
                "writer modified sidecar on first write");
        const auto first = sp4_test::bytes(output);
        const auto json = jsoncons::json::parse(first);
        require(!json.at("layers").at(2).contains("material"), "omit empty material");
        require(json.at("external_refs").size() == 1, "deduplicate external refs");
        require(json.at("external_refs").at(0).at("path").as<std::string>() == "copper.polyset",
                "preserve external path spelling");
        // Python independently verifies the digest over the exact sidecar bytes.
        std::ofstream script(dir / "hash.py");
        script << "import hashlib,json,pathlib\np=pathlib.Path(__file__).parent\n"
                  "j=json.loads((p/'out.json').read_text())\n"
                  "assert "
                  "j['external_refs'][0]['sha256']==hashlib.sha256((p/"
                  "'copper.polyset').read_bytes()).hexdigest()\n";
        script.close();
        const auto command = "python3 '" + (dir / "hash.py").string() + "'";
        require(std::system(command.c_str()) == 0, "Python SHA-256 verification");
        const auto before = sp4_test::bytes(dir / "copper.polyset");
        require(bool(chipcae::importer::write_stack(model, output)), "second write failed");
        require(first == sp4_test::bytes(output), "serialization must be deterministic");
        require(before == sp4_test::bytes(dir / "copper.polyset"), "writer modified sidecar");
        // Reject references outside the output root before creating an output file.
        const auto rejected = dir / "rejected.json";
        for (const auto& file : {std::string("../outside.polyset"),
                                 (dir.parent_path() / "outside.polyset").string()}) {
            auto invalid = model;
            invalid.layers[0].copper->polygons->file = file;
            require(!chipcae::importer::write_stack(invalid, rejected),
                    "invalid sidecar reference must be rejected");
            require(!std::filesystem::exists(rejected), "invalid reference created output");
        }
        auto absolute = model;
        for (auto& layer : absolute.layers)
            if (layer.copper && layer.copper->polygons)
                layer.copper->polygons->file = (dir / "copper.polyset").string();
        require(bool(chipcae::importer::write_stack(absolute, rejected)),
                "absolute sidecar failed");
        const auto absolute_json = jsoncons::json::parse(sp4_test::bytes(rejected));
        require(absolute_json["external_refs"][0]["path"].as<std::string>() ==
                    (dir / "copper.polyset").string(),
                "absolute spelling changed");
        std::filesystem::remove(rejected);
        std::filesystem::create_symlink(dir.parent_path(), dir / "escape");
        auto invalid = model;
        invalid.layers[0].copper->polygons->file = "escape/outside.polyset";
        require(!chipcae::importer::write_stack(invalid, rejected), "reject symlink escape");
        require(!std::filesystem::exists(rejected), "symlink escape created output");
        std::filesystem::remove(dir / "copper.polyset");
        require(!chipcae::importer::write_stack(model, output), "missing sidecar must be an error");
        require(first == sp4_test::bytes(output), "missing sidecar changed existing output");
        require(!chipcae::importer::write_stack(model, rejected), "missing sidecar must fail");
        require(!std::filesystem::exists(rejected), "missing sidecar created output");
        if (std::filesystem::exists("/dev/full")) {
            // Exercise a write that opens successfully but fails during flush.
            const auto failed =
                chipcae::importer::write_stack(chipcae::mesh::StackModel{}, "/dev/full");
            require(!failed, "full device must fail serialization");
            require(failed.error().reason == "INPUT_INVALID: cannot write stack",
                    "write failure must preserve its serialization error");
        }
#else
        require(false, "roundtrip test requires jsoncons headers");
#endif
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
