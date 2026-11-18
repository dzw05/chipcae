#include "chipcae/geometry/ecad/ipc2581_zip.hpp"

#include "ipc2581_stream.hpp"
#include "odbpp/archive.hpp"

#include <algorithm>
#include <cctype>

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
    if (lower(file.extension().string()) != ".zip" || std::filesystem::is_directory(file))
        return rejected("IMPORT_REJECTED: IPC ZIP reader requires a .zip file");
    detail::ArchiveReader archive(file);
    // ArchiveReader treats a valid empty archive as an error. IPC selection still
    // needs to report the absence of XML (including directory-only archives).
    if (!archive.open_error().empty() && archive.open_error() != "IMPORT_REJECTED: empty input")
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
        for (std::size_t i = 0; i < std::min<std::size_t>(20, count); ++i) {
            if (i)
                error += ", ";
            error += matches.empty() ? archive.entries()[i].path : matches[i];
        }
        if (count > 20)
            error += " … (total " + std::to_string(count) + ")";
        return rejected(std::move(error));
    }
    const auto& name = matches.front();
    if (!archive.is_supported(name)) {
        for (const auto& reason : archive.unsupported())
            if (reason.element.rfind(name + ": ", 0) == 0)
                return rejected("IMPORT_REJECTED: " + reason.element);
        return rejected("IMPORT_REJECTED: unsupported XML entry " + name);
    }
    detail::Ipc2581StreamParser parser;
    bool first = true;
    std::string error;
    const bool streamed = archive.for_each_line(
        name,
        [&](std::string_view line) {
            if (!first && !parser.feed("\n"))
                return false;
            first = false;
            return parser.feed(line);
        },
        error);
    auto result = parser.finish();
    if (!streamed && !error.empty())
        return rejected(std::move(error));
    return result;
}
} // namespace chipcae::geometry::ecad
