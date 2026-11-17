#include "archive.hpp"

#include <algorithm>
namespace chipcae::geometry::ecad::detail {
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
            if (e.path == "steps/" + job.step + "/layers/" + layer.name + "/attrlist" &&
                !attrs(e.path, job.attrs.layer[layer.name]))
                return false;
        }
    }
    return true;
}
} // namespace chipcae::geometry::ecad::detail
