#include "chipcae/geometry/import_report.hpp"

#include <iostream>

using namespace chipcae::geometry;

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "expected directory and mode\n";
        return 1;
    }
    const std::filesystem::path directory(argv[1]);
    const std::string mode(argv[2]);
    if (mode != "four" && mode != "empty" && mode != "replacement")
        return 1;
    ImportReportHeader header;
    header.L = 1;
    header.t_min = .1;
    header.tolerances = {0.000001, 0.00002, 0.0003, 0.004};
    header.b_tier_accepted_all = mode == "replacement";
    std::vector<Defect> defects;
    const std::string ids[] = {"caseA", "casea", "duplicate", "duplicate"};
    if (mode != "empty") {
        for (int tier = 0; tier < (mode == "four" ? 4 : 1); ++tier) {
            Defect d;
            d.tier = static_cast<Tier>(tier);
            d.reason = DefectReason::TOLERANCE_RANGE_UNSUPPORTED;
            d.dimension = std::string("L") + std::to_string(tier);
            d.size = 0.125 + tier * 0.25;
            d.defect_id = mode == "four" ? ids[tier] : "replacement";
            const auto suffix = std::to_string(tier);
            d.kind = "kind_" + suffix;
            d.suggestion = "repair_" + suffix;
            d.action = "action_" + suffix;
            d.rejection_reason = "rejection_" + suffix;
            d.material_ambiguous = tier % 2 != 0;
            defects.push_back(d);
        }
    }
    std::string error;
    if (!write_import_report(directory, header, defects, &error)) {
        std::cerr << error << '\n';
        return 1;
    }
    std::cout << (directory / "import_report.json").string() << '\n'
              << (directory / "import_report.csv").string() << '\n';
}
