#pragma once
#include "chipcae/geometry/ecad/ipc2581.hpp"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace chipcae::geometry::ecad {
using ThicknessOverrides = std::map<std::string, double>;
struct ThicknessRecord {
    std::string layer;
    double thickness_m = 0;
    std::string source;
    std::optional<double> odb_value_m;
};
struct OdbReadOptions {
    ThicknessOverrides overrides;
};
// Overrides match ASCII case-insensitively; folded duplicates are rejected.
// Records are cleared on entry and returned once per retained layer in stack order,
// with original layer spelling. No records are published on failure.
ImportResult read_odbpp(const std::filesystem::path& path);
ImportResult read_odbpp(const std::filesystem::path& path,
                        const OdbReadOptions& options,
                        std::vector<ThicknessRecord>* records = nullptr);
} // namespace chipcae::geometry::ecad
