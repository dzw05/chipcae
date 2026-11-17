#include "chipcae/geometry/ecad/odbpp.hpp"
#include "map.hpp"
#include "odb_job.hpp"

namespace chipcae::geometry::ecad::detail {
ImportResult map_geometry(const OdbJob&) {
    ImportResult result;
    result.error = "IMPORT_REJECTED: not implemented";
    return result;
}
} // namespace chipcae::geometry::ecad::detail

namespace chipcae::geometry::ecad::odbpp_detail {
Expected<EcadModel> map_odb(const OdbJob&, const MapOptions&) {
    ImportResult result;
    result.error = "IMPORT_REJECTED: not implemented";
    return result;
}
} // namespace chipcae::geometry::ecad::odbpp_detail
