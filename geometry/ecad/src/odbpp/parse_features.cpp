#include "chipcae/geometry/ecad/odbpp.hpp"
#include "odb_job.hpp"

namespace chipcae::geometry::ecad::detail {
ImportResult parse_features(const OdbJob&) {
    ImportResult result;
    result.error = "IMPORT_REJECTED: not implemented";
    return result;
}
} // namespace chipcae::geometry::ecad::detail
