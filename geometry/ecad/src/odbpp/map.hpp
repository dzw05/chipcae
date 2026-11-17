#pragma once
#include "chipcae/geometry/ecad/ipc2581.hpp"
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
    // "odb", "override", or "missing"; the caller attaches override filename provenance.
    std::string source;
    // Original positive ODB value when a differing override was supplied but ignored.
    std::optional<double> odb_value_m;
};
struct MapOptions {
    const ThicknessOverrides* overrides = nullptr;
    std::vector<ThicknessRecord>* records = nullptr;
};
// Shared private implementation helpers; no public API additions.
std::string fold_ascii(std::string);
double unit_m(const OdbJob&);
void map_stackup(const OdbJob&, const MapOptions&, ImportResult&);
ImportResult map_odb(const OdbJob&, const MapOptions& = {});
} // namespace chipcae::geometry::ecad::odbpp_detail
