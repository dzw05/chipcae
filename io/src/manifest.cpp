#include "chipcae/io/manifest.hpp"

#include <fstream>
#include <stdexcept>
#include <utility>

namespace chipcae::io {

void ResultManifest::add(ManifestEntry entry) {
    entries_.push_back(std::move(entry));
}

void ResultManifest::write(const std::filesystem::path& path) const {
    std::ofstream out(path);
    if (!out) {
        throw std::runtime_error("Failed to write result manifest: " + path.string());
    }
    out << "name,path,type\n";
    for (const auto& entry : entries_) {
        out << entry.name << ',' << entry.path << ',' << entry.type << '\n';
    }
}

} // namespace chipcae::io
