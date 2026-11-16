#include "chipcae/geometry/geometry.hpp"

#include <stdexcept>
#include <utility>

namespace chipcae::geometry {

Geometry Geometry::empty() {
    return Geometry{};
}

Geometry Geometry::from_stl(const std::filesystem::path& path) {
    if (path.empty()) {
        throw std::invalid_argument("STL path must not be empty");
    }
    throw std::runtime_error("STL import is not implemented yet: " + path.string());
}

Real Geometry::signed_distance(Real /*x*/, Real /*y*/, Real /*z*/) const {
    // Positive: outside the solid. A real SDF will replace this placeholder.
    return Real{1};
}

} // namespace chipcae::geometry
