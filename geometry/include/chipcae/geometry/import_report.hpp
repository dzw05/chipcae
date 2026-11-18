#pragma once

#include "chipcae/geometry/import_thresholds.hpp"

#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace chipcae::geometry {

struct ImportReportHeader {
    Real L = 0;
    Real t_min = 0;
    ImportTolerances tolerances;
    bool b_tier_accepted_all = false;
};

// Optional assembly extension, serialized by the same writer as common fields.
struct ImportReportParts {
    std::map<std::string, std::vector<std::string>> split_layers;
    std::vector<std::pair<std::string, std::vector<std::string>>> rewrite_targets;
    std::vector<std::string> material_approx;
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
bool write_import_report(const std::filesystem::path& directory,
                         const ImportReportHeader& header,
                         const std::vector<Defect>& defects,
                         std::string* error = nullptr,
                         const ImportReportParts* parts = nullptr);
std::string tier_name(Tier tier);
std::string defect_id(const Defect& defect);

} // namespace chipcae::geometry
