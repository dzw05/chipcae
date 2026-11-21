#include "chipcae/geometry/assembly.hpp"

// Supply deterministic STEP data and erase body IDs after the real merge.
// Keep placement, layer splitting, rejection policy and reporting in the real CLI.
namespace chipcae::geometry {
bool floating_test_step_available() noexcept;
StepImportResult floating_test_import_step(const std::filesystem::path&, const StepImportOptions&);
SliceResult floating_test_slice(const StepImportResult&, std::string_view, const SliceOptions&);
Expected<MergeResult>
floating_test_merge(StackModel, const std::vector<PlacedPart>&, const MergeOptions&);
} // namespace chipcae::geometry
#define step_import_available floating_test_step_available
#define import_step floating_test_import_step
#define slice_prismatic floating_test_slice
#define merge_parts floating_test_merge
#include "assembly_seam_support.hpp"
#undef merge_parts
#undef slice_prismatic
#undef import_step
#undef step_import_available

#include <chrono>
#include <cmath>
#include <iostream>
#include <sstream>

#if defined(CHIPCAE_ENABLE_JSONCONS) && defined(CHIPCAE_ENABLE_ECAD)
#include "../../../kernel/src/stack_writer.cpp"

namespace chipcae::geometry {
bool floating_test_step_available() noexcept {
    return true;
}
StepImportResult floating_test_import_step(const std::filesystem::path&, const StepImportOptions&) {
    StepImportResult step;
    step.body_names = {"lid"};
    return step;
}
SliceResult floating_test_slice(const StepImportResult&, std::string_view, const SliceOptions&) {
    SlabSection section;
    section.outer.pts = {{.001, .001}, {.004, .001}, {.004, .003}, {.001, .003}};
    return {{{0, .125, {section}}, {.125, .375, {section}}}, {}};
}
Expected<MergeResult> floating_test_merge(StackModel base,
                                          const std::vector<PlacedPart>& placed,
                                          const MergeOptions& options) {
    auto result = merge_parts(std::move(base), placed, options);
    if (!result)
        return result;
    for (auto& layer : result.value().stack.layers) {
        for (auto& region : layer.regions)
            region.body.clear();
        // Model independent rounding of a serialized cavity thickness.
        if (placed.at(0).id.ends_with("_rounded") && layer.material.empty() &&
            layer.regions.empty())
            layer.thickness = std::nextafter(layer.thickness, 1.);
    }
    return result;
}
} // namespace chipcae::geometry

bool reported_gap(bool above, bool rounded) {
    using namespace chipcae;
    using J = jsoncons::json;
    const auto dir = std::filesystem::temp_directory_path() /
                     ("floating-gap-" +
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir);
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::filesystem::remove_all(path); }
    } cleanup{dir};
    std::filesystem::copy_file(std::filesystem::path(CHIPCAE_SOURCE_DIR) /
                                   "tests/integration/ecad/data/board_2layer.xml",
                               dir / "board.xml");
    std::ofstream(dir / "map.json") << R"({"FR4":"fr4_eq","copper":"copper"})";
    const std::string id =
        std::string(above ? "top_lid" : "bottom_lid") + (rounded ? "_rounded" : "");
    // The synthetic board is .035 + 1.6 + .035 mm thick. The cavity is .125 m.
    const Real origin = above ? .00167 + .125 : -.5;
    J document = J::parse(R"({"schema":"chipcae.assembly/0.1",
        "board":{"path":"board.xml","format":"ipc2581","material_map":"map.json"},
        "parts":[{"id":"lid","step":"lid.step","material":"solid",
                  "placement":{"frame":"step","origin":[0,0,0]}}]})");
    document["parts"][0]["id"] = id;
    document["parts"][0]["placement"]["origin"][2] = origin;
    std::ofstream(dir / "assembly.json") << document;
    importer::ImportOptions options;
    options.assembly = dir / "assembly.json";
    options.output = dir / "out.stack.json";
    options.polyset = dir / "board.polyset";
    options.report = dir / "defects.json";
    std::ostringstream errors;
    auto* previous = std::cerr.rdbuf(errors.rdbuf());
    const int exit = importer::run_assembly(options);
    std::cerr.rdbuf(previous);
    std::ifstream input(options.report);
    const auto report = J::parse(input);
    const std::string expected = "INPUT_INVALID: gap without bond leaves " + id + " floating";
    if (exit != 14 || std::filesystem::exists(options.output) ||
        report["code"].as<std::string>() != "INPUT_INVALID" ||
        report["message"].as<std::string>() != expected || errors.str() != expected + "\n") {
        std::cerr << id << " (rounded=" << rounded << "): expected exit 14 and " << expected
                  << "; got exit " << exit << " and " << report << '\n';
        return false;
    }
    return true;
}
#endif

int main() {
    using namespace chipcae;
    bool reports_ok = true;
#if defined(CHIPCAE_ENABLE_JSONCONS) && defined(CHIPCAE_ENABLE_ECAD)
    for (const bool above : {true, false})
        for (const bool rounded : {false, true})
            reports_ok = reported_gap(above, rounded) && reports_ok;
#endif
    mesh::Layer board{};
    board.name = "board";
    board.material = "fr4_eq";
    board.thickness = .5;
    mesh::StackModel base;
    base.layers = {board};

    mesh::Layer extension{};
    extension.name = "extension";
    extension.material = "solid";
    extension.thickness = .375;
    mesh::Layer gap{};
    gap.name = "gap";
    gap.thickness = .125;
    mesh::StackModel merged;
    merged.layers = {extension, gap, board};
    // The board's bottom face is at .5 in the merged stack. The empty
    // interval below it ends at .375 in the merged frame, and at -.125
    // in the board-relative coordinates retained by place().
    const geometry::PlacedPart unrelated{"unrelated", "solid", {{-.5, -.25, {}}}};
    const geometry::PlacedPart lid{"lid", "solid", {{-.5, -.375, {}}, {-.375, -.125, {}}}};
    if (importer::floating_gap_part(merged, base, {unrelated, lid}) != "lid") {
        std::cerr << "below-board fallback must use the offset board face\n";
        return 1;
    }

    auto nearby = lid;
    nearby.slabs.back().z1 = std::nextafter(-.125, 0.);
    if (importer::floating_gap_part(merged, base, {unrelated, nearby}) != "lid") {
        std::cerr << "below-board fallback must tolerate endpoint roundoff\n";
        return 1;
    }
    nearby.slabs.back().z1 = -.1249;
    if (importer::floating_gap_part(merged, base, {unrelated, nearby})) {
        std::cerr << "below-board fallback must reject a distinct endpoint\n";
        return 1;
    }
    // Independent decimal thickness sums reconstruct .3 and .1 with rounding.
    board.thickness = .1;
    base.layers = {board};
    extension.thickness = .1;
    gap.thickness = .2;
    merged.layers = {board, gap, extension};
    const geometry::PlacedPart top{"top", "solid", {{.3, .4, {}}}};
    if (importer::floating_gap_part(merged, base, {top}) != "top") {
        std::cerr << "above-board fallback must tolerate independently summed thicknesses\n";
        return 1;
    }
    merged.layers = {extension, gap, board};
    const geometry::PlacedPart bottom{"bottom", "solid", {{-.3, -.2, {}}}};
    if (importer::floating_gap_part(merged, base, {bottom}) != "bottom") {
        std::cerr << "below-board fallback must tolerate independently summed thicknesses\n";
        return 1;
    }
    board.thickness = .5;
    base.layers = {board};
    extension.thickness = .375;
    gap.thickness = .125;
    merged.layers = {extension, board, gap, extension};
    const geometry::PlacedPart upper{"upper", "solid", {{.625, 1., {}}}};
    if (importer::floating_gap_part(merged, base, {upper}) != "upper") {
        std::cerr << "above-board fallback must retain the original slab frame with extensions\n";
        return 1;
    }
    merged.layers = {extension, gap, board};
    mesh::Inclusion region{};
    merged.layers.front().regions.push_back(region);
    auto unnamed = lid;
    unnamed.id.clear();
    if (importer::floating_gap_part(merged, base, {unnamed, lid}) != "lid") {
        std::cerr << "region matching must not select an empty part id\n";
        return 1;
    }
    // A region body at the cavity still takes precedence over the fallback.
    region.body = "known";
    merged.layers.front().regions.push_back(region);
    auto known = lid;
    known.id = "known";
    if (importer::floating_gap_part(merged, base, {lid, known}) != "known") {
        std::cerr << "region provenance must take precedence over endpoint fallback\n";
        return 1;
    }
    return reports_ok ? 0 : 1;
}
