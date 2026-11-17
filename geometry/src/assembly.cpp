#include "chipcae/geometry/assembly.hpp"

#include <stdexcept>
namespace chipcae::geometry {
Expected<PlacedPart> place(std::vector<Slab>, const Placement&, const StackModel&) {
    throw std::logic_error("not implemented: place");
}
Expected<MergeResult> merge_parts(StackModel, const std::vector<PlacedPart>&, const MergeOptions&) {
    throw std::logic_error("not implemented: merge_parts");
}
} // namespace chipcae::geometry
