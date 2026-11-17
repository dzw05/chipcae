#include "chipcae/geometry/import_report.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>

using namespace chipcae::geometry;
static void check(bool ok, const char* text) {
    if (!ok) {
        std::cerr << text << '\n';
        std::exit(1);
    }
}

int main() {
    std::vector<Defect> ds;
    for (int tier = 0; tier < 4; ++tier)
        for (int i = 0; i < 10; ++i) {
            Defect d;
            d.tier = static_cast<Tier>(tier);
            d.reason = DefectReason::TOLERANCE_RANGE_UNSUPPORTED;
            d.dimension = "L";
            d.size = i;
            d.defect_id = std::to_string(tier) + "-" + std::to_string(i);
            d.suggestion = "repair";
            ds.push_back(d);
        }
    auto r = apply_repair_policy(ds, {});
    check(r.result == ImportResult::Rejected && r.defects.size() == 40,
          "tier B confirmation and complete rows");
    for (const auto& d : r.defects)
        if (d.tier != Tier::Weld)
            check(!d.action.empty() || !d.rejection_reason.empty(), "no silent defects");
    std::vector<std::string> ids;
    for (int i = 0; i < 10; ++i)
        ids.push_back("2-" + std::to_string(i));
    std::vector<Defect> without_c(ds.begin(), ds.begin() + 30);
    r = apply_repair_policy(without_c, {});
    check(r.result == ImportResult::ConfirmationRequired && r.defects.size() == 30,
          "unconfirmed tier B requires confirmation and writes all rows");
    r = apply_repair_policy(without_c, ids);
    check(r.result == ImportResult::Success, "confirmed tier B accepted");
    const auto dir = std::filesystem::temp_directory_path() / "chipcae-import-report-test";
    ImportReportHeader h;
    h.L = 1;
    h.t_min = .1;
    check(write_import_report(dir, h, r.defects), "report writing");
    check(std::filesystem::file_size(dir / "import_report.json") > 0 &&
              std::filesystem::file_size(dir / "import_report.csv") > 0,
          "reports present");
    std::filesystem::remove_all(dir);
}
