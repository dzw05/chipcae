#include "chipcae/geometry/assembly.hpp"

#include <stdexcept>
namespace chipcae::geometry {
mesh::Expected<PlacedPart> place(std::vector<Slab>, const Placement&, const mesh::StackModel&) {
    throw std::logic_error("not implemented: place");
}
mesh::Expected<MergeResult>
merge_parts(mesh::StackModel, const std::vector<PlacedPart>&, const MergeOptions&) {
    throw std::logic_error("not implemented: merge_parts");
}
} // namespace chipcae::geometry
