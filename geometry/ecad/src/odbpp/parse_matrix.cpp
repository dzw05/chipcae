#include "archive.hpp"

#include <set>

namespace chipcae::geometry::ecad::detail {
bool parse_matrix(const ArchiveReader& reader, OdbJob& job, std::string& error) {
    std::string block;
    OdbLayer layer;
    std::set<std::string> names;
    bool valid = true;
    const bool ok = reader.for_each_line(
        "matrix/matrix",
        [&](std::string_view raw) {
            auto line = trim(raw);
            if (line.empty() || line[0] == '#')
                return true;
            if (line.starts_with("LAYER") && line.find('{') != std::string::npos) {
                block = "LAYER";
                layer = {};
                return true;
            }
            if (line.starts_with("STEP") && line.find('{') != std::string::npos) {
                block = "STEP";
                return true;
            }
            if (line == "}") {
                if (block == "LAYER") {
                    if (layer.name.empty()) {
                        error = "IMPORT_REJECTED: matrix layer without NAME";
                        valid = false;
                        return false;
                    }
                    if (!names.insert(layer.name).second) {
                        error = "IMPORT_REJECTED: duplicate layer " + layer.name;
                        valid = false;
                        return false;
                    }
                    job.layers.push_back(layer);
                }
                block.clear();
                return true;
            }
            auto eq = line.find('=');
            if (eq == std::string::npos)
                return true;
            auto key = trim(std::string_view(line).substr(0, eq)),
                 value = trim(std::string_view(line).substr(eq + 1));
            if (key == "UNITS")
                job.units = lower(value) == "inch" ? OdbUnits::inch : OdbUnits::mm;
            if (block == "LAYER") {
                if (key == "NAME")
                    layer.name = lower(value);
                else if (key == "CONTEXT")
                    layer.context = lower(value);
                else if (key == "TYPE")
                    layer.type = lower(value);
                else if (key == "POLARITY")
                    layer.polarity = lower(value);
                else if (key == "ROW")
                    layer.row = std::stoi(value);
                else if (key == "START_NAME")
                    layer.start_name = lower(value);
                else if (key == "END_NAME")
                    layer.end_name = lower(value);
            }
            return true;
        },
        error);
    if (!ok || !valid)
        return false;
    if (!block.empty()) {
        error = "IMPORT_REJECTED: unterminated matrix block";
        return false;
    }
    return true;
}
} // namespace chipcae::geometry::ecad::detail
