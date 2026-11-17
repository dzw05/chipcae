#include "chipcae/geometry/assembly.hpp"

#include <stdexcept>
namespace chipcae::geometry {
Expected<PlacedPart> place(std::vector<Slab>, const Placement&, const mesh::StackModel&) {
    throw std::logic_error("not implemented: place");
}
Expected<MergeResult>
merge_parts(mesh::StackModel, const std::vector<PlacedPart>&, const MergeOptions&) {
    throw std::logic_error("not implemented: merge_parts");
}
} // namespace chipcae::geometry
