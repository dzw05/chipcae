#include "chipcae/geometry/ecad/odbpp.hpp"

namespace chipcae::geometry::ecad {
ImportResult read_odbpp(const std::filesystem::path&) {
    ImportResult result;
    result.error = "IMPORT_REJECTED: not implemented";
    return result;
}
} // namespace chipcae::geometry::ecad
