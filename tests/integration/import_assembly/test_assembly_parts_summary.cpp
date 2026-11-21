#include "assembly_seam_support.hpp"

#include <iostream>

int main() {
    using namespace chipcae;
    mesh::StackModel base, merged;
    mesh::Layer lower{}, upper{};
    lower.name = "L";
    upper.name = "U";
    mesh::Inclusion region{};
    region.name = "R";
    lower.regions.push_back(region);
    base.layers = {lower, upper};
    auto first = lower, second = lower;
    first.name = "L#0";
    second.name = "L#1";
    first.regions[0].name = "R#0";
    second.regions[0].name = "R#1";
    merged.layers = {first, second, upper};
    importer::ImportReportParts parts;
    importer::summarize_assembly_parts(base, merged, parts);
    if (parts.split_layers !=
        std::map<std::string, std::vector<std::string>>{{"L", {"L#0", "L#1"}}})
        return 1;
    const decltype(parts.rewrite_targets) expected{{"layer:L", {"layer:L#0", "layer:L#1"}},
                                                   {"region:R", {"region:R#0", "region:R#1"}},
                                                   {"interface:L|U", {"interface:L#1|U"}}};
    const bool targets_differ = parts.rewrite_targets != expected;
    if (targets_differ)
        std::cerr << "in-memory rewrite_targets: expected interface:L|U -> "
                     "[interface:L#1|U] only\n";
#if defined(CHIPCAE_ENABLE_JSONCONS)
    const auto directory = std::filesystem::temp_directory_path() /
                           ("sp4-summary-" + std::to_string(std::random_device{}()));
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::filesystem::remove_all(path); }
    } cleanup{directory};
    if (!importer::write_assembly_report(directory, {}, {}, nullptr, &parts))
        return 1;
    std::ifstream input(directory / "import_report.json");
    const auto report = jsoncons::json::parse(input);
    const auto want = jsoncons::json::parse(R"({"split_layers":{"L":["L#0","L#1"]},
        "rewrite_targets":[{"from":"layer:L","to":["layer:L#0","layer:L#1"]},
        {"from":"region:R","to":["region:R#0","region:R#1"]},
        {"from":"interface:L|U","to":["interface:L#1|U"]}],
        "material_approx":[]})");
    if (report["parts"] != want) {
        std::cerr << "serialized parts: expected " << want << ", got " << report["parts"] << '\n';
        return 1;
    }
#endif
    return targets_differ ? 1 : 0;
}
