#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace chipcae::io {

struct ManifestEntry {
    std::string name;
    std::string path;
    std::string type;
};

class ResultManifest {
public:
    void add(ManifestEntry entry);
    const std::vector<ManifestEntry>& entries() const noexcept { return entries_; }
    void write(const std::filesystem::path& path) const;

private:
    std::vector<ManifestEntry> entries_;
};

} // namespace chipcae::io
