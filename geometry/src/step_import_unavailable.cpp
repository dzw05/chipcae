#include "chipcae/geometry/step_import.hpp"

#include <stdexcept>

namespace chipcae::geometry {
bool step_import_available() noexcept {
    return false;
}
StepImportResult import_step(const std::filesystem::path&, const StepImportOptions&) {
    throw std::runtime_error("STEP import requires CHIPCAE_ENABLE_OCCT=ON");
}
} // namespace chipcae::geometry
