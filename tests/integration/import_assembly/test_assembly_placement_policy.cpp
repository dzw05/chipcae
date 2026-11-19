#include "assembly_seam_support.hpp"

int main() {
    using namespace chipcae;
    std::vector<geometry::Defect> defects;
    const std::string message = "INPUT_INVALID: attachment gap is invalid";
    geometry::Expected<geometry::PlacedPart> failed = geometry::ImportThresholdError{message};
    if (importer::assembly_placement_result(failed, "lid", "copper", defects) ||
        defects.size() != 1 || defects[0].tier != geometry::Tier::C ||
        defects[0].reason != geometry::DefectReason::PART_ATTACH_INVALID ||
        defects[0].defect_id != "lid/attach_invalid" || defects[0].details != message)
        return 1;
    for (bool accept_all : {false, true}) {
        const auto policy =
            geometry::apply_repair_policy(defects, {"lid/attach_invalid"}, accept_all);
        if (policy.result != geometry::ImportResult::Rejected
#if defined(CHIPCAE_ENABLE_JSONCONS)
            || importer::assembly_error_exit("IMPORT_REJECTED: C-tier defects") != 7
#endif
        )
            return 1;
    }
    auto part =
        importer::assembly_placement_result(geometry::PlacedPart{}, "ok", "copper", defects);
    if (!part || part->id != "ok" || part->material != "copper" || defects.size() != 1)
        return 1;

    geometry::Defect b{};
    b.tier = geometry::Tier::B;
    b.defect_id = "synthetic/thin-layer";
    if (geometry::apply_repair_policy({b}, {}).result !=
            geometry::ImportResult::ConfirmationRequired ||
#if defined(CHIPCAE_ENABLE_JSONCONS)
        importer::assembly_error_exit(
            "IMPORT_CONFIRMATION_REQUIRED: B-tier defects require acceptance") != 6 ||
#endif
        geometry::apply_repair_policy({b}, {b.defect_id}).result !=
            geometry::ImportResult::Success ||
        geometry::apply_repair_policy({b}, {}, true).result != geometry::ImportResult::Success)
        return 1;
}
