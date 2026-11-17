#include "parser.hpp"

#include <algorithm>
#include <charconv>
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
            if (key == "UNITS") {
                auto units = lower(value);
                if (units != "mm" && units != "inch") {
                    error = "IMPORT_REJECTED: unsupported matrix units " + value;
                    valid = false;
                    return false;
                }
                job.units = units == "inch" ? OdbUnits::inch : OdbUnits::mm;
            }
            if (block == "LAYER") {
                if (key == "NAME")
                    layer.name = value;
                else if (key == "CONTEXT")
                    layer.context = lower(value);
                else if (key == "TYPE")
                    layer.type = lower(value);
                else if (key == "POLARITY")
                    layer.polarity = lower(value);
                else if (key == "ROW") {
                    const auto [end, ec] =
                        std::from_chars(value.data(), value.data() + value.size(), layer.row);
                    if (ec != std::errc{} || end != value.data() + value.size()) {
                        error = "IMPORT_REJECTED: malformed matrix ROW matrix/matrix";
                        valid = false;
                        return false;
                    }
                } else if (key == "START_NAME")
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

ImportResult read_odbpp_job(const std::filesystem::path& input, OdbJob& job) {
    ImportResult result;
    result.model.id = "odbpp";
    job = {};
    try {
        ArchiveReader reader(input);
        result.model.report.unsupported = reader.unsupported();
        result.error = reader.open_error();
        if (!result.error.empty())
            return result;
        if (!parse_matrix(reader, job, result.error))
            return result;
        std::vector<std::string> steps;
        for (const auto& entry : reader.entries()) {
            if (!entry.path.starts_with("steps/"))
                continue;
            auto tail = entry.path.substr(6);
            auto slash = tail.find('/');
            if (slash == std::string::npos && !entry.is_dir)
                continue;
            auto name = tail.substr(0, slash);
            if (!name.empty() && std::find(steps.begin(), steps.end(), name) == steps.end())
                steps.push_back(name);
        }
        if (steps.empty()) {
            result.error = "IMPORT_REJECTED: no step";
            return result;
        }
        auto pcb = std::find(steps.begin(), steps.end(), "pcb");
        job.step = pcb == steps.end() ? steps.front() : *pcb;
        if (pcb == steps.end() || steps.size() > 1)
            result.model.report.unsupported.push_back(
                {"IMPORT_UNSUPPORTED_ELEMENT", "selected step " + job.step, 0});
        if (!parse_misc(reader, job, result.error))
            return result;
        if (!parse_features(reader, job, result))
            return result;
    } catch (const std::exception& e) {
        result.error = "IMPORT_REJECTED: " + std::string(e.what());
    }
    return result;
}
} // namespace chipcae::geometry::ecad::detail
