#pragma once

#include "chipcae/geometry/import_thresholds.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace chipcae::geometry {

struct ImportReportHeader {
    Real L = 0;
    Real t_min = 0;
    ImportTolerances tolerances;
    bool b_tier_accepted_all = false;
    // Assembly imports populate this even when no interfaces were welded.
    std::optional<std::int64_t> welded_interfaces;
};

enum class ImportResult { Success, ConfirmationRequired, Rejected };
struct RepairPolicyResult {
    ImportResult result = ImportResult::Success;
    std::vector<Defect> defects;
    std::vector<std::string> warnings;
};

RepairPolicyResult apply_repair_policy(std::vector<Defect> defects,
                                       const std::vector<std::string>& accepted_ids,
                                       bool accept_all_b = false);
// Callers normalize IDs before policy matching and pass the same final defects to all writers.
bool write_import_report(const std::filesystem::path& directory,
                         const ImportReportHeader& header,
                         const std::vector<Defect>& defects,
                         std::string* error = nullptr);
std::string tier_name(Tier tier);
std::string defect_id(const Defect& defect);
// Fills empty IDs and suffixes duplicates in order, reserving original IDs. Idempotent.
void deduplicate_defect_ids(std::vector<Defect>& defects);
std::string defect_size_unit(const Defect& defect);

} // namespace chipcae::geometry
