#pragma once

#include "chipcae/geometry/diagnostics.hpp"

#include <filesystem>
#include <memory>
#include <vector>

namespace chipcae::geometry {
// All lengths are metres. h_min is supplied by the caller's intended mesh.
struct StepImportOptions {
    ImportTolerances tolerances{1e-9, 1e-6, 1e-4, 0};
    Real h_min = 1e-3;
};
namespace detail {
struct StepShape;
}
struct StepImportResult {
    std::array<Real, 6> bbox{}; // xmin,ymin,zmin,xmax,ymax,zmax, in metres
    std::vector<Defect> defects;
    std::vector<std::string> body_names;
    // Immutable ownership of the original topology and its source history for E5.
    // No OCCT types cross the public header boundary.
    std::shared_ptr<const detail::StepShape> source;
};
bool step_import_available() noexcept;
// Throws invalid_argument for invalid options, runtime_error for unavailable OCCT
// or a failed read/transfer. Never repairs, sews, or tessellates the source.
StepImportResult import_step(const std::filesystem::path&, const StepImportOptions& = {});
} // namespace chipcae::geometry
