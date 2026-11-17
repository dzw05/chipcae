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

Geometry Geometry::from_step(const std::filesystem::path& path, const StepImportOptions& options) {
    Geometry result;
    result.step_ = std::make_shared<StepImportResult>(import_step(path, options));
    result.name_ = path.filename().string();
    return result;
}

Real Geometry::signed_distance(Real /*x*/, Real /*y*/, Real /*z*/) const {
    if (step_)
        throw std::logic_error("STEP signed-distance evaluation is not implemented");
    return 1;
}

} // namespace chipcae::geometry
