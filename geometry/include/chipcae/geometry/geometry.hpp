#pragma once

#include "chipcae/core/types.hpp"
#include "chipcae/geometry/step_import.hpp"

#include <filesystem>
#include <string>

namespace chipcae::geometry {

/// Owns an immutable STEP import report and source. Empty geometry has no boundary.
class Geometry {
public:
    static Geometry empty();
    static Geometry from_stl(const std::filesystem::path& path);

    static Geometry from_step(const std::filesystem::path&, const StepImportOptions& = {});
    const StepImportResult* step() const noexcept { return step_.get(); }

    const std::string& name() const noexcept { return name_; }
    Real signed_distance(Real x, Real y, Real z) const;

private:
    std::shared_ptr<const StepImportResult> step_;
    std::string name_{"empty"};
};

} // namespace chipcae::geometry
