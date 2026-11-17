#include "chipcae/geometry/ecad/ipc2581.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;
using Paths = std::set<std::string>;
void require(bool condition, const std::string& message) {
    if (!condition)
        throw std::runtime_error(message);
}
std::string quote(const std::string& value) {
    std::string result = "'";
    for (char c : value)
        result += c == '\'' ? "'\\''" : std::string(1, c);
    return result + "'";
}
std::vector<std::string> listing(const fs::path& archive, bool verbose) {
    const auto command =
        "cmake -E tar " + std::string(verbose ? "tvf " : "tf ") + quote(archive.string());
    FILE* pipe = popen(command.c_str(), "r");
    require(pipe != nullptr, "Cannot run " + command);
    std::string output;
    std::array<char, 4096> buffer{};
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe))
        output += buffer.data();
    const int status = pclose(pipe);
    require(status == 0, "Archive listing failed: " + command);
    std::vector<std::string> lines;
    std::istringstream stream(output);
    for (std::string line; std::getline(stream, line);)
        lines.push_back(line);
    return lines;
}
Paths archive_paths(const fs::path& archive) {
    require(fs::is_regular_file(archive), "Missing archive: " + archive.string());
    const auto names = listing(archive, false);
    const auto metadata = listing(archive, true);
    require(names.size() == metadata.size(), "Inconsistent archive listing");
    Paths paths;
    for (std::size_t i = 0; i < names.size(); ++i) {
        // Verbose tar output distinguishes regular files from directories and symlinks.
        if (metadata[i].empty() || metadata[i][0] != '-')
            continue;
        auto name = names[i];
        std::replace(name.begin(), name.end(), '\\', '/');
        while (name.starts_with("./"))
            name.erase(0, 2);
        require(paths.insert(name).second, "Duplicate archive file: " + name);
    }
    require(!paths.empty(), "Archive has no regular files");
    return paths;
}
Paths normalize(Paths paths, const Paths& directory) {
    if (paths == directory)
        return paths;
    const auto slash = paths.begin()->find('/');
    if (slash == std::string::npos)
        return paths;
    const auto prefix = paths.begin()->substr(0, slash + 1);
    Paths stripped;
    for (const auto& path : paths) {
        if (!path.starts_with(prefix))
            return paths;
        stripped.insert(path.substr(prefix.size()));
    }
    return stripped;
}
Paths archive_paths(const fs::path& archive) {
    require(fs::is_regular_file(archive), "Missing archive: " + archive.string());
    const auto names = listing(archive, false);
    const auto details = listing(archive, true);
    require(names.size() == details.size(), "Archive listings disagree: " + archive.string());
    Paths paths;
    std::set<std::string> roots;
    bool all_nested = true;
    for (std::size_t i = 0; i < names.size(); ++i) {
        auto name = names[i];
        std::replace(name.begin(), name.end(), '\\', '/');
        while (name.starts_with("./"))
            name.erase(0, 2);
        if (name.empty())
            continue;
        const auto slash = name.find('/');
        roots.insert(name.substr(0, slash));
        if (details[i].empty() || details[i].front() != '-')
            continue; // verbose listing identifies directories and symbolic links
        all_nested = all_nested && slash != std::string::npos;
        paths.insert(name);
    }
    require(!paths.empty(), "Archive has no regular files: " + archive.string());
    if (roots.size() == 1 && all_nested) {
        Paths stripped;
        for (const auto& path : paths)
            stripped.insert(path.substr(path.find('/') + 1));
        return stripped;
    }
    return paths;
}
void odb_fixture(const fs::path& root) {
    const auto job = root / "kicad4l";
    require(fs::is_regular_file(job / "matrix/matrix"), "Missing fixture matrix/matrix");
    require(fs::is_regular_file(job / "misc/attrlist"), "Missing fixture misc/attrlist");
    Paths directory;
    int profiles = 0;
    for (const auto& entry : fs::recursive_directory_iterator(job)) {
        if (!fs::is_regular_file(entry.symlink_status()))
            continue;
        const auto relative = entry.path().lexically_relative(job);
        directory.insert(relative.generic_string());
        if (std::distance(relative.begin(), relative.end()) == 3 && *relative.begin() == "steps" &&
            relative.filename() == "profile")
            ++profiles;
    }
    require(profiles == 1, "Expected exactly one steps/<step>/profile");
    for (const auto* name : {"kicad4l.tgz", "kicad4l.zip"}) {
        const auto paths = archive_paths(root / name);
        require(paths == directory,
                std::string(name) + " regular-file paths differ from directory");
    }
}
void ipc_fixture(const fs::path& root) {
    const auto result = chipcae::geometry::ecad::read_ipc2581(root / "kicad4l.ipc2581.xml");
    require(static_cast<bool>(result), "IPC fixture import failed: " + result.error);
    const auto copper = std::count_if(result.model.layers.begin(),
                                      result.model.layers.end(),
                                      [](const auto& layer) { return layer.copper; });
    require(copper == 4, "Expected exactly four copper layers; got " + std::to_string(copper));
}
} // namespace
int main(int argc, char** argv) {
    try {
        const auto root = fs::path(CHIPCAE_TEST_SOURCE_DIR) / "tests/integration/ecad/data/odbpp";
        require(argc == 2, "Expected odb or ipc selector");
        const std::string mode = argv[1];
        if (mode == "odb")
            odb_fixture(root);
        else if (mode == "ipc")
            ipc_fixture(root);
        else
            throw std::runtime_error("Unknown fixture selector");
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
    return 0;
}
