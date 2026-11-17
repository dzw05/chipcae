#include "chipcae/geometry/part_slice.hpp"

#include <stdexcept>

// SP0 contract stub; SP2 supplies the implementation.
namespace chipcae::geometry {
SliceResult slice_prismatic(const StepImportResult&, std::string_view, const SliceOptions&) {
    throw std::logic_error("not implemented: slice_prismatic");
}
} // namespace chipcae::geometry
