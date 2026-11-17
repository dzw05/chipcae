#pragma once
#include "chipcae/geometry/step_import.hpp"

#include <string_view>
namespace chipcae::geometry {
// TEST-FIRST compile stubs; replace with the frozen SP0 interface during implementation.
struct Slab {};
struct SliceOptions {};
struct SliceResult {};
inline SliceResult slice_prismatic(const StepImportResult&, std::string_view, const SliceOptions&) {
    return {};
}
} // namespace chipcae::geometry
