#pragma once

#include "chipcae/core/types.hpp"

#include <filesystem>
#include <string>

namespace chipcae::geometry {

/// Geometry container. STL/STEP import and SDF evaluation will be filled in later.
class Geometry {
public:
    static Geometry empty();
    static Geometry from_stl(const std::filesystem::path& path);

    const std::string& name() const noexcept { return name_; }
    Real signed_distance(Real x, Real y, Real z) const;

private:
    std::string name_{"empty"};
};

} // namespace chipcae::geometry
