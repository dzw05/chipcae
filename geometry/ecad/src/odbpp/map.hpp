#pragma once
#include "chipcae/geometry/ecad/ipc2581.hpp"
#include "chipcae/geometry/import_thresholds.hpp"
#include "odb_job.hpp"

#include <map>
#include <optional>
#include <string>
#include <vector>
namespace chipcae::geometry::ecad::odbpp_detail {
using detail::OdbJob;
using ThicknessOverrides = std::map<std::string, double>;
struct ThicknessRecord {
    std::string layer;
    double thickness_m = 0;
    // Internal source: "odb", "override", or "missing". The frozen mapper inputs
    // contain no filename; E-ODB-3 must expand "override" to "override:<filename>"
    // when converting these records to its public report types.
    std::string source;
    // Original positive ODB thickness when a conflicting override is applied.
    std::optional<double> odb_value_m;
};
struct MapOptions {
    // Frozen E-ODB-2 contract: exactly these two fields. File provenance belongs
    // to the E-ODB-3 caller, which reads the override file, not to this mapper.
    const ThicknessOverrides* overrides = nullptr;
    std::vector<ThicknessRecord>* records = nullptr;
};
// Shared private implementation helpers; no public API additions.
std::string fold_ascii(std::string);
double unit_m(const OdbJob&);
void map_stackup(const OdbJob&, const MapOptions&, ImportResult&);
Expected<EcadModel> map_odb(const OdbJob&, const MapOptions& = {});
} // namespace chipcae::geometry::ecad::odbpp_detail
