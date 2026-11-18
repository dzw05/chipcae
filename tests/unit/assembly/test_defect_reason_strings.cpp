#include "chipcae/geometry/import_thresholds.hpp"

#include <iostream>
#include <string_view>

int main() {
    using chipcae::geometry::Defect;
    using chipcae::geometry::DefectReason;
    using chipcae::geometry::to_string;

    int failures = 0;
    if (std::string_view(to_string(DefectReason::TOLERANCE_RANGE_UNSUPPORTED)) !=
        "TOLERANCE_RANGE_UNSUPPORTED") {
        std::cerr << "FAIL: wrong string for TOLERANCE_RANGE_UNSUPPORTED\n";
        ++failures;
    }
    if (std::string_view(to_string(DefectReason::THIN_LAYER_MERGED)) != "THIN_LAYER_MERGED") {
        std::cerr << "FAIL: wrong string for THIN_LAYER_MERGED\n";
        ++failures;
    }
    if (std::string_view(to_string(DefectReason::PART_UNMAPPED)) != "PART_UNMAPPED") {
        std::cerr << "FAIL: wrong string for PART_UNMAPPED\n";
        ++failures;
    }
    if (std::string_view(to_string(DefectReason::PART_NOT_PRISMATIC)) != "PART_NOT_PRISMATIC") {
        std::cerr << "FAIL: wrong string for PART_NOT_PRISMATIC\n";
        ++failures;
    }
    if (std::string_view(to_string(DefectReason::PART_TILTED)) != "PART_TILTED") {
        std::cerr << "FAIL: wrong string for PART_TILTED\n";
        ++failures;
    }
    if (std::string_view(to_string(DefectReason::PART_OVERLAP)) != "PART_OVERLAP") {
        std::cerr << "FAIL: wrong string for PART_OVERLAP\n";
        ++failures;
    }
    if (std::string_view(to_string(DefectReason::PART_OUTSIDE_OUTLINE)) != "PART_OUTSIDE_OUTLINE") {
        std::cerr << "FAIL: wrong string for PART_OUTSIDE_OUTLINE\n";
        ++failures;
    }
    if (std::string_view(to_string(DefectReason::PART_ATTACH_INVALID)) != "PART_ATTACH_INVALID") {
        std::cerr << "FAIL: wrong string for PART_ATTACH_INVALID\n";
        ++failures;
    }
    if (std::string_view(to_string(DefectReason::OCCT_UNAVAILABLE)) != "OCCT_UNAVAILABLE") {
        std::cerr << "FAIL: wrong string for OCCT_UNAVAILABLE\n";
        ++failures;
    }

    Defect defect;
    if (!defect.details.empty()) {
        std::cerr << "FAIL: default Defect.details must be empty\n";
        ++failures;
    }
    return failures ? 1 : 0;
}
