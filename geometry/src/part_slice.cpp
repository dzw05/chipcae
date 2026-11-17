#include "chipcae/geometry/part_slice.hpp"

#include <stdexcept>

namespace chipcae::geometry {
SliceResult slice_prismatic(const StepImportResult&, std::string_view, const SliceOptions&) {
    throw std::logic_error("not implemented: slice_prismatic");
}
} // namespace chipcae::geometry
