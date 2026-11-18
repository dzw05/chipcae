#pragma once

#include "chipcae/geometry/import_thresholds.hpp"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace chipcae::geometry::ecad {
struct EcadModel;
}
namespace chipcae::importer {
struct LayerSourceRow {
    std::string layer;
    std::string material_key;
    std::string material_id;
    std::string thickness_source;
};
struct ParsedStackOverride {
    std::map<std::string, double> thicknesses;
    std::map<std::string, std::string> materials;
    std::optional<std::string> source;
};
std::string fold_layer_name(std::string name);
chipcae::geometry::Expected<ParsedStackOverride>
read_stack_override(const std::filesystem::path& file);
} // namespace chipcae::importer
