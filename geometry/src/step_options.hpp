#pragma once

#include "chipcae/geometry/step_import.hpp"

#include <cmath>
#include <stdexcept>

namespace chipcae::geometry::detail {
inline void validate_step_options(const std::filesystem::path& path,
                                  const StepImportOptions& options) {
    const auto& t = options.tolerances;
    if (path.empty() || !std::isfinite(options.h_min) || options.h_min <= 0 ||
        !std::isfinite(t.eps_weld) || !std::isfinite(t.tau_A) || !std::isfinite(t.tau_B) ||
        t.eps_weld <= 0 || t.tau_A < t.eps_weld || t.tau_B < t.tau_A)
        throw std::invalid_argument("invalid STEP path or inspection scales");
}
} // namespace chipcae::geometry::detail
