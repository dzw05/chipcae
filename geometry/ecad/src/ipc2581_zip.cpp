#include "chipcae/geometry/ecad/ipc2581_zip.hpp"

#include "ipc2581_stream.hpp"
#include "odbpp/archive.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>
#include <vector>

namespace chipcae::geometry::ecad {
namespace {
ImportResult rejected(std::string error) {
    ImportResult result;
    result.error = std::move(error);
    return result;
}
std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}
} // namespace
ImportResult read_ipc2581_zip(const std::filesystem::path& file) {
    detail::ArchiveReader archive(file);
    if (!archive.open_error().empty())
        return rejected(archive.open_error());
    std::vector<std::string> matches;
    for (const auto& entry : archive.entries()) {
        const auto& name = entry.path;
        const auto basename = name.substr(name.find_last_of('/') + 1);
        if (entry.is_dir || name.empty() || name.back() == '/' || name.rfind("__MACOSX/", 0) == 0 ||
            basename.rfind("._", 0) == 0)
            continue;
        if (basename.size() >= 4 && lower(basename.substr(basename.size() - 4)) == ".xml")
            matches.push_back(name);
    }
    if (matches.size() != 1) {
        std::string error = matches.empty() ? "IMPORT_REJECTED: no .xml entry: "
                                            : "IMPORT_REJECTED: multiple .xml entries: ";
        const auto count = matches.empty() ? archive.entries().size() : matches.size();
        const auto listed = matches.empty() ? std::min<std::size_t>(20, count) : count;
        for (std::size_t i = 0; i < listed; ++i) {
            if (i)
                error += ", ";
            error += matches.empty() ? archive.entries()[i].path : matches[i];
        }
        if (!matches.empty())
            error += " （共 " + std::to_string(count) + " 个）";
        else if (count > 20)
            error += " …（共 " + std::to_string(count) + " 个）";
        return rejected(std::move(error));
    }
    const auto& name = matches.front();
    if (!archive.is_supported(name)) {
        std::string error;
        const bool streamed =
            archive.for_each_line(name, [](std::string_view) { return true; }, error);
        if (streamed || error.empty())
            error = "IMPORT_REJECTED: unsupported entry: " + name;
        return rejected(std::move(error));
    }
    detail::Ipc2581StreamParser parser;
    bool first = true;
    bool feed_failed = false;
    std::string error;
    const bool streamed = archive.for_each_line(
        name,
        [&](std::string_view line) {
            if (!first && !parser.feed("\n")) {
                feed_failed = true;
                return false;
            }
            first = false;
            feed_failed = !parser.feed(line);
            return !feed_failed;
        },
        error);
    if (!feed_failed && !streamed) {
        if (error.empty())
            error = "IMPORT_REJECTED: archive read failed: " + name;
        return rejected(std::move(error));
    }
    return parser.finish();
}
} // namespace chipcae::geometry::ecad
