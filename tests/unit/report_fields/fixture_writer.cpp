#include "chipcae/geometry/import_accept.hpp"
#include "chipcae/geometry/import_report.hpp"

#include <iostream>
#include <limits>

using namespace chipcae::geometry;

int main(int argc, char** argv) {
    constexpr auto reason = DefectReason::TOLERANCE_RANGE_UNSUPPORTED;
    // Expose the named enum value to the Python field expectations.
    if (argc == 2 && std::string(argv[1]) == "--reason-value") {
        std::cout << static_cast<int>(reason) << '\n';
        return 0;
    }
    if (argc != 3) {
        std::cerr << "expected directory and mode\n";
        return 1;
    }
    const std::filesystem::path directory(argv[1]);
    const std::string mode(argv[2]);

    ImportReportHeader header;
    header.L = 1;
    header.t_min = .1;
    header.tolerances = {0.000001, 0.00002, 0.0003, 0.004};
    header.b_tier_accepted_all = mode == "replacement";
    std::vector<Defect> defects;
    constexpr Tier tiers[] = {Tier::Weld, Tier::A, Tier::B, Tier::C};
    const std::string ids[] = {"caseA", "casea", "duplicate", "duplicate"};
    if (mode != "empty") {
        for (int tier = 0; tier < (mode == "four" ? 4 : 1); ++tier) {
            Defect d{};
            d.tier = tiers[tier];
            d.reason = reason;
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
    if (mode == "assembly-zero" || mode == "assembly-count") {
        defects.clear();
        header.welded_interfaces = mode == "assembly-zero" ? 0 : 9007199254740993LL;
    } else if (mode == "ids" || mode == "ids-twice" || mode == "generated" || mode == "replay") {
        defects.clear();
        const std::vector<std::string> ordered = {
            "foo", "foo", "foo#2", "foo", "part/1", "part#status", "part#2"};
        const auto ids_to_use = mode == "generated" ? std::vector<std::string>{"", ""}
                                : mode == "replay"  ? std::vector<std::string>{"foo", "foo"}
                                                    : ordered;
        for (const auto& id : ids_to_use) {
            Defect d{};
            d.tier = Tier::B;
            d.reason = reason;
            d.defect_id = id;
            d.dimension = "element";
            d.suggestion = "synthetic repair";
            defects.push_back(d);
        }
    } else if (mode == "escaping") {
        auto& d = defects.front();
        const std::string special =
            std::string("quote\" backslash\\ newline\n tab\t return\r nul") + char(0) + " unit" +
            char(0x1f);
        d.defect_id = special;
        d.kind = special;
        d.dimension = special;
        d.suggestion = special;
        d.action = special;
        d.rejection_reason = special;
    } else if (mode == "units") {
        defects.clear();
        const std::string dimensions[] = {"element",
                                          "",
                                          "unknown",
                                          "L",
                                          "t_min",
                                          "measured",
                                          "delta",
                                          "L",
                                          "measured",
                                          "element"};
        for (int i = 0; i < 10; ++i) {
            Defect d{};
            d.tier = Tier::A;
            d.reason = reason;
            d.dimension = dimensions[i];
            d.defect_id = "unit_" + std::to_string(i);
            d.size = i == 7   ? std::numeric_limits<double>::quiet_NaN()
                     : i == 8 ? std::numeric_limits<double>::infinity()
                     : i == 9 ? -std::numeric_limits<double>::infinity()
                              : 0.125 + i * 0.25;
            defects.push_back(d);
        }
    } else if (mode != "four" && mode != "empty" && mode != "replacement") {
        return 1;
    }
    std::string error;
    int exit_code = 0;
    if (mode == "replay") {
        const auto list = read_accept_file(directory / "accept.json", &error);
        if (!list) {
            std::cerr << error << '\n';
            return 1;
        }
        // Exercise the production normalization point in policy, without a test-side rename.
        auto result = apply_repair_policy(defects, accepted_ids(*list));
        for (const auto& warning : result.warnings)
            std::cerr << warning << '\n';
        defects = std::move(result.defects);
        exit_code = result.result == ImportResult::ConfirmationRequired ? 6
                    : result.result == ImportResult::Rejected           ? 7
                                                                        : 0;
    } else {
        deduplicate_defect_ids(defects);
        if (mode == "ids-twice")
            deduplicate_defect_ids(defects);
    }
    if (!write_import_report(directory, header, defects, &error)) {
        std::cerr << error << '\n';
        return 1;
    }
    std::cout << (directory / "import_report.json").string() << '\n'
              << (directory / "import_report.csv").string() << '\n';
    return exit_code;
}
