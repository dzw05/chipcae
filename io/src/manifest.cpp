#include "chipcae/io/manifest.hpp"

#include <fstream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace chipcae::io {
namespace {
void write_string(std::ostream& out, std::string_view value) {
    constexpr char hex[] = "0123456789abcdef";
    out << '"';
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') {
            out << '\\' << static_cast<char>(c);
        } else if (c < 0x20) {
            out << "\\u00" << hex[c >> 4] << hex[c & 0xf];
        } else {
            out << static_cast<char>(c);
        }
    }
    out << '"';
}
} // namespace

void ResultManifest::add(ManifestEntry entry) {
    entries_.push_back(std::move(entry));
}

void ResultManifest::write(const std::filesystem::path& path) const {
    std::ofstream out(path);
    if (!out) {
        throw std::runtime_error("Failed to write result manifest: " + path.string());
    }
    out << "{\n  \"manifest_version\": 1,\n  \"entries\": [";
    bool first = true;
    for (const auto& entry : entries_) {
        if (!first) {
            out << ',';
        }
        first = false;
        out << "\n    {\"name\": ";
        write_string(out, entry.name);
        out << ", \"path\": ";
        write_string(out, entry.path);
        out << ", \"type\": ";
        write_string(out, entry.type);
        out << '}';
    }
    out << "\n  ]\n}\n";
    out.close();
    if (!out) {
        throw std::runtime_error("Failed to finish result manifest: " + path.string());
    }
}

} // namespace chipcae::io
