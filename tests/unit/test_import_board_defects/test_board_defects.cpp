#include "../../../kernel/src/import_ecad_defects.hpp"

#include <array>
#include <cmath>
#include <iostream>
#include <string>
#include <utility>

namespace {
using chipcae::geometry::Defect;
using chipcae::geometry::Tier;
namespace ecad = chipcae::geometry::ecad;
namespace defects = chipcae::importer::ecad_defects;

// Project tolerance: absolute 1e-10 plus relative 5.5e-6.
bool close(double actual, double expected) {
    return std::abs(actual - expected) <= 1e-10 + 5.5e-6 * std::abs(expected);
}

bool same_defect(const Defect& a, const Defect& b) {
    return a.tier == b.tier && a.reason == b.reason && a.dimension == b.dimension &&
           close(a.size, b.size) && a.suggestion == b.suggestion && a.defect_id == b.defect_id &&
           a.kind == b.kind && a.action == b.action && a.rejection_reason == b.rejection_reason &&
           a.material_ambiguous == b.material_ambiguous && a.details == b.details;
}
} // namespace

int main() {
    int failures = 0;
    const auto check = [&](bool condition, const std::string& message) {
        if (!condition) {
            std::cerr << "board defects: " << message << '\n';
            ++failures;
        }
    };
    ecad::ImportReport report;
    for (const auto* element : {"MysteryA",
                                "Circle",
                                "SkippedLayer:F.Silkscreen:SILKSCREEN",
                                "ViaPlatingMissing:A:B:12",
                                "MysteryA"})
        report.unsupported.push_back({"IMPORT_UNSUPPORTED_ELEMENT", element, 2});
    const auto actual = defects::board_defects(report, false);
    const std::array<std::pair<std::string, Tier>, 4> expected{
        {{"board/MysteryA/2", Tier::B},
         {"board/Circle/2", Tier::C},
         {"board/via_plating_missing/A-B", Tier::B},
         {"board/MysteryA/2/1", Tier::B}}};
    check(actual.size() == expected.size(),
          "IPC wrapper must return 4 defects (extension B, geometry C, via B, duplicate B); got " +
              std::to_string(actual.size()));
    for (std::size_t i = 0; i < actual.size(); ++i) {
        const auto& d = actual[i];
        check(d.defect_id.starts_with("board/"), "every ID must have the board/ prefix");
        check(d.kind == "IMPORT_UNSUPPORTED_ELEMENT", "unsupported kind must be preserved");
        if (i < expected.size()) {
            check(d.defect_id == expected[i].first, "ordered ID must be " + expected[i].first);
            check(d.tier == expected[i].second, "wrong tier for " + expected[i].first);
        }
    }
    if (actual.size() > 2)
        check(close(actual[2].size, 12), "missing via plating must preserve count 12");

    std::vector<defects::SkippedRow> skipped;
    const auto classified = defects::classify_unsupported(report, skipped, false);
    check(skipped.size() == 1, "classifier must expose exactly one skipped layer");
    if (skipped.size() == 1)
        check(skipped[0].name == "F.Silkscreen" && skipped[0].function == "SILKSCREEN",
              "skipped layer name/function must be preserved");
    check(classified.size() == actual.size(), "wrapper and classifier counts must agree");
    for (std::size_t i = 0; i < actual.size() && i < classified.size(); ++i)
        check(same_defect(actual[i], classified[i]), "wrapper must preserve every defect field");

    check(defects::board_defects({}, false).empty(), "empty IPC report must yield no defects");
    check(defects::board_defects({}, true).empty(), "empty ODB report must yield no defects");
    ecad::ImportReport odb;
    odb.unsupported.push_back({"IMPORT_UNSUPPORTED_ELEMENT", "MysteryA", 2});
    const auto unknown = defects::board_defects(odb, true);
    check(unknown.size() == 1, "unknown ODB diagnostic must yield one C defect");
    if (unknown.size() == 1) {
        check(unknown[0].tier == Tier::C, "unknown ODB diagnostic must remain C");
        check(unknown[0].defect_id == "board/MysteryA/2", "ODB ID must preserve source line");
        check(unknown[0].kind == "IMPORT_UNSUPPORTED_ELEMENT", "ODB kind must be preserved");
    }
    return failures ? 1 : 0;
}
