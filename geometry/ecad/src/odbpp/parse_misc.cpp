#include "archive.hpp"

#include <algorithm>
namespace chipcae::geometry::ecad::detail {
// Preserve matrix names in the model and prefer literal directory spelling.
// Exporters may use uppercase matrix names with lowercase on-disk paths.
std::string
layer_directory(const ArchiveReader& reader, const OdbJob& job, const std::string& name) {
    auto base = "steps/" + job.step + "/layers/" + name + "/";
    const bool literal =
        std::any_of(reader.entries().begin(), reader.entries().end(), [&](const auto& entry) {
            return entry.path == base.substr(0, base.size() - 1) || entry.path.starts_with(base);
        });
    return literal ? base : "steps/" + job.step + "/layers/" + lower(name) + "/";
}
bool parse_misc(const ArchiveReader& reader, OdbJob& job, std::string& error) {
    auto attrs = [&](const std::string& path, std::map<std::string, std::string>& values) {
        return reader.for_each_line(
            path,
            [&](std::string_view raw) {
                auto line = trim(raw);
                if (line.empty() || line[0] == '#')
                    return true;
                auto eq = line.find('=');
                if (eq != std::string::npos)
                    values[trim(std::string_view(line).substr(0, eq))] =
                        trim(std::string_view(line).substr(eq + 1));
                return true;
            },
            error);
    };
    for (const auto& e : reader.entries()) {
        if (e.is_dir)
            continue;
        if (e.path == "misc/attrlist" && !attrs(e.path, job.attrs.job))
            return false;
        for (const auto& layer : job.layers) {
            if (e.path == layer_directory(reader, job, layer.name) + "attrlist" &&
                !attrs(e.path, job.attrs.layer[layer.name]))
                return false;
        }
    }
    return true;
}
} // namespace chipcae::geometry::ecad::detail
