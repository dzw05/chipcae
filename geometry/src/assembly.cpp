#include "chipcae/geometry/assembly.hpp"

#include <stdexcept>

// SP0 contract stub; SP3 supplies the implementation.
namespace chipcae::geometry {
Expected<PlacedPart> place(std::vector<Slab>, const Placement&, const StackModel&) {
    throw std::logic_error("not implemented: place");
}
Expected<MergeResult> merge_parts(StackModel, const std::vector<PlacedPart>&, const MergeOptions&) {
    throw std::logic_error("not implemented: merge_parts");
}
} // namespace chipcae::geometry
