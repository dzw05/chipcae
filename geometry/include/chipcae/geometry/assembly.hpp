#pragma once
#include "chipcae/geometry/part_slice.hpp"
#include "chipcae/schema/stack.hpp"

#include <vector>
namespace chipcae::geometry {
// TEST-FIRST compile stubs; no placement or merge behavior implemented.
struct Placement {};
struct PlacedPart {};
struct MergeOptions {};
struct MergeResult {};
inline Expected<PlacedPart> place(std::vector<Slab>, const Placement&, const mesh::StackModel&) {
    return PlacedPart{};
}
inline Expected<MergeResult>
merge_parts(mesh::StackModel, const std::vector<PlacedPart>&, const MergeOptions&) {
    return MergeResult{};
}
} // namespace chipcae::geometry
