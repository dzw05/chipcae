#pragma once

#include "chipcae/geometry/step_import.hpp"

#include <functional>

namespace chipcae::geometry::detail {
// Internal injection seam for checks that fail before inspection completes.
StepImportResult import_step_with_inspection(const std::filesystem::path&,
                                             const StepImportOptions&,
                                             const std::function<void()>& before_inspection);
} // namespace chipcae::geometry::detail
