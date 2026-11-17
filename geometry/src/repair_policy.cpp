#include "chipcae/geometry/import_report.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace chipcae::geometry {
std::string tier_name(Tier tier) {
    switch (tier) {
    case Tier::Weld:
        return "Weld";
    case Tier::A:
        return "A";
    case Tier::B:
        return "B";
    case Tier::C:
        return "C";
    }
    return "C";
}

std::string defect_id(const Defect& d) {
    if (!d.defect_id.empty())
        return d.defect_id;
    std::ostringstream s;
    s << tier_name(d.tier) << ':' << static_cast<unsigned>(d.reason) << ':' << d.dimension << ':'
      << std::setprecision(17) << d.size;
    return s.str();
}

RepairPolicyResult apply_repair_policy(std::vector<Defect> defects,
                                       const std::vector<std::string>& accepted_ids,
                                       bool accept_all_b) {
    RepairPolicyResult out;
    for (const auto& id : accepted_ids) {
        if (std::none_of(defects.begin(), defects.end(), [&](const Defect& d) {
                return defect_id(d) == id;
            }))
            out.warnings.push_back("confirmation references unknown defect_id: " + id);
    }
    for (auto& d : defects) {
        if (d.tier == Tier::Weld) {
            if (d.action.empty())
                d.action = "welded or deduplicated";
        } else if (d.tier == Tier::A) {
            if (d.action.empty())
                d.action = d.suggestion.empty() ? "automatic repair applied" : d.suggestion;
        } else if (d.tier == Tier::B) {
            const auto id = defect_id(d);
            if (accept_all_b ||
                std::find(accepted_ids.begin(), accepted_ids.end(), id) != accepted_ids.end()) {
                if (d.action.empty())
                    d.action = d.suggestion.empty() ? "tier B repair applied" : d.suggestion;
            } else {
                d.rejection_reason = "tier B defect requires explicit confirmation";
                out.result = ImportResult::ConfirmationRequired;
            }
        } else {
            d.rejection_reason = d.material_ambiguous ? "material ownership is ambiguous"
                                                      : "tier C defect is rejected";
            out.result = ImportResult::Rejected;
        }
    }
    out.defects = std::move(defects);
    return out;
}
} // namespace chipcae::geometry
