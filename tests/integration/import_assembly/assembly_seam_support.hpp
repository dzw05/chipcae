#pragma once
#include "chipcae/geometry/diagnostics.hpp"
#include "chipcae/geometry/import_report.hpp"
#include "chipcae/schema/stack.hpp"

#include <string>
#include <vector>

// Private orchestration seams for deterministic synthetic diagnostics. Including
// the TU avoids exporting these helpers as installed geometry APIs. Until SP4 is
// present, these minimal test-local stubs compile and fail the behavior assertions.
#if __has_include("../../../kernel/src/import_assembly.cpp")
#include "../../../kernel/src/import_assembly.cpp"
#else
namespace chipcae::importer {
inline std::vector<geometry::Defect> map_step_defects(const std::string&,
                                                      const std::vector<geometry::StepDefect>&) {
    return {};
}
inline chipcae::mesh::Expected<bool> inline_assembly_outline(chipcae::mesh::StackModel&,
                                                             const chipcae::mesh::Polyset&) {
    return true;
}
inline int assembly_error_exit(const std::string&) {
    return 0;
}
} // namespace chipcae::importer
#endif
