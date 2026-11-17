#include "chipcae/geometry/part_slice.hpp"

#include <stdexcept>

// SP0 contract stub; SP2 supplies the implementation.
namespace chipcae::geometry {
SliceResult slice_prismatic(const StepImportResult&, std::string_view, const SliceOptions&) {
#ifdef CHIPCAE_ENABLE_OCCT
    throw std::logic_error("not implemented: slice_prismatic");
#else
    Defect defect{};
    defect.tier = Tier::C;
    defect.reason = DefectReason::OCCT_UNAVAILABLE;
    return {{}, {std::move(defect)}};
#endif
}
} // namespace chipcae::geometry
