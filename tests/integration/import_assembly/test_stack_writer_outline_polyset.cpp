#include "assembly_seam_support.hpp"
#include "writer_test_support.hpp"

#include <chrono>

int main() {
    using namespace chipcae::mesh;
    using sp4_test::require;
    try {
#ifdef SP4_HAS_JSONCONS
        const auto dir =
            std::filesystem::temp_directory_path() /
            ("sp4-outline-" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(dir);
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::filesystem::remove_all(path); }
        } cleanup{dir};
        const auto source = std::filesystem::path(CHIPCAE_SOURCE_DIR);
        const auto schema = source / "schema/stack-0.2.schema.json";
        Polyset poly{1e-6,
                     {{"outline", {0, 5}, {0, 0, 20000, 0, 20000, 15000, 15000, 20000, 0, 20000}}}};
        require(bool(write_polyset(dir / "outline.polyset", poly)), "write outline fixture");
        auto json = jsoncons::json::parse(
            sp4_test::bytes(source / "schema/examples/fcbga_demo.stack.json"));
        jsoncons::json ref = jsoncons::json::object();
        ref["path"] = "outline.polyset";
        ref["sha256"] = sha256(sp4_test::bytes(dir / "outline.polyset"));
        json["external_refs"] = jsoncons::json::array();
        json["external_refs"].push_back(ref);
        std::ofstream(dir / "original.json") << json;
        auto base = load_stack(dir / "original.json", schema);
        require(bool(base), base ? "" : base.error().reason);
        require(base.value().outline.polygon.empty(), "fixture must require outline inlining");
        auto sidecar = read_polyset(dir / "outline.polyset");
        require(bool(sidecar), "read outline fixture");
        auto inlined = chipcae::importer::inline_assembly_outline(base.value(), sidecar.value());
        require(bool(inlined), "single ring outline must inline successfully");
        require(base.value().outline.polygon.size() == 5,
                "assembly load must inline all five polyset outline points before merge");
        for (std::size_t i = 0; i < 5; ++i)
            for (std::size_t j = 0; j < 2; ++j)
                require(base.value().outline.polygon[i][j] ==
                            poly.layers[0].coordinates[2 * i + j] * 1e-6,
                        "outline coordinates must equal DBU coordinates times dbu_m exactly");
        auto result = chipcae::importer::write_stack(base.value(), dir / "rewritten.json");
        require(bool(result), "write inlined model");
        auto loaded = load_stack(dir / "rewritten.json", schema);
        require(bool(loaded), loaded ? "" : loaded.error().reason);
        require(loaded.value().outline.polygon == base.value().outline.polygon,
                "outline points changed when reloaded");
        const auto command =
            "python3 '" + (source / "tests/integration/import_assembly/outline_solve.py").string() +
            "' '" + dir.string() + "'";
        require(std::system(command.c_str()) == 0, "optional solve equivalence failed");
        base.value().outline.polygon.clear();
        auto invalid = poly;
        invalid.layers[0].ring_offsets = {0, 3, 5};
        require(!chipcae::importer::inline_assembly_outline(base.value(), invalid),
                "multiple rings must return CUT_UNSUPPORTED");
        invalid = poly;
        invalid.layers.push_back(poly.layers.front());
        require(!chipcae::importer::inline_assembly_outline(base.value(), invalid),
                "multiple outline layers must return CUT_UNSUPPORTED");
        require(base.value().outline.polygon.empty(), "invalid outlines must not mutate the stack");
#else
        require(false, "outline acceptance requires jsoncons headers");
#endif
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
