#pragma once
#include "chipcae/geometry/ecad/ipc2581.hpp"
#include "odb_job.hpp"

#include <map>
#include <optional>
#include <string>
#include <vector>
namespace chipcae::geometry::ecad::odbpp_detail {
using detail::OdbJob;
// Compile-only bridge until the prerequisite supplies the Expected abstraction.
template <class T>
using Expected = ImportResult;
using ThicknessOverrides = std::map<std::string, double>;
struct ThicknessRecord {
    std::string layer;
    double thickness_m = 0;
    std::string source;
    std::optional<double> odb_value_m;
};
struct MapOptions {
    const ThicknessOverrides* overrides = nullptr;
    std::vector<ThicknessRecord>* records = nullptr;
};
Expected<EcadModel> map_odb(const OdbJob&, const MapOptions& = {});
} // namespace chipcae::geometry::ecad::odbpp_detail
