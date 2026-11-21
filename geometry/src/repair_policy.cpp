#include "chipcae/geometry/import_report.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace chipcae::geometry {
void deduplicate_defect_ids(std::vector<Defect>& defects) {
    // Reserve future original IDs so suffix allocation never renames a unique ID.
    std::unordered_set<std::string> reserved, emitted;
    for (auto& d : defects) {
        d.defect_id = defect_id(d);
        reserved.insert(d.defect_id);
    }
    std::unordered_map<std::string, std::size_t> next_suffix;
    for (auto& d : defects) {
        if (emitted.insert(d.defect_id).second)
            continue;
        const auto base = d.defect_id;
        auto it = next_suffix.try_emplace(base, 2).first;
        auto& suffix = it->second;
        std::string candidate;
        do {
            candidate = base + "#" + std::to_string(suffix++);
        } while (reserved.contains(candidate) || emitted.contains(candidate));
        d.defect_id = std::move(candidate);
        emitted.insert(d.defect_id);
    }
}

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
    deduplicate_defect_ids(defects);
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
