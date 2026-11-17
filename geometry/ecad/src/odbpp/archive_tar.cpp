#include "archive.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <limits>
#include <set>

namespace chipcae::geometry::ecad::detail {
namespace {
std::string field(std::string_view s) {
    return std::string(s.substr(0, s.find('\0')));
}
bool octal(std::string_view s, std::uint64_t& v) {
    v = 0;
    bool ended = false;
    for (unsigned char c : s) {
        if (c == 0 || c == ' ') {
            if (v)
                ended = true;
            continue;
        }
        if (ended || c < '0' || c > '7' || v > (UINT64_MAX - 7) / 8)
            return false;
        v = v * 8 + c - '0';
    }
    return true;
}
} // namespace
bool index_tar(const std::filesystem::path& path,
               const ArchiveLimits& limits,
               std::vector<ArchiveEntry>& entries,
               std::vector<std::uint64_t>& offsets,
               std::vector<UnsupportedElement>& unsupported,
               std::string& error) {
    std::ifstream input(path, std::ios::binary);
    std::string header, extension, long_name, pax_path;
    std::uint64_t remain = 0, padding = 0, position = 0, total = 0, count = 0, pax_size = 0;
    bool has_pax_size = false, done = false;
    char type = 0;
    int zeroes = 0;
    std::set<std::string> seen;
    auto fail = [&](std::string reason) {
        error = "IMPORT_REJECTED: " + reason;
        return false;
    };
    auto metadata = [&]() {
        if (type == 'L') {
            long_name = field(extension);
            if (!long_name.empty() && long_name.back() == '\n')
                long_name.pop_back();
        }
        if (type == 'x') {
            std::size_t i = 0;
            while (i < extension.size()) {
                auto space = extension.find(' ', i);
                if (space == std::string::npos)
                    return fail("invalid pax record");
                std::size_t len = 0;
                auto r = std::from_chars(extension.data() + i, extension.data() + space, len);
                if (r.ec != std::errc{} || r.ptr != extension.data() + space ||
                    len <= space - i + 1 || len > extension.size() - i)
                    return fail("invalid pax length");
                auto record = std::string_view(extension).substr(space + 1, i + len - space - 2);
                auto eq = record.find('=');
                if (eq != std::string_view::npos) {
                    auto key = record.substr(0, eq), value = record.substr(eq + 1);
                    if (key == "path")
                        pax_path = std::string(value);
                    if (key == "size") {
                        auto n =
                            std::from_chars(value.data(), value.data() + value.size(), pax_size);
                        if (n.ec != std::errc{} || n.ptr != value.data() + value.size())
                            return fail("invalid pax size");
                        has_pax_size = true;
                    }
                }
                i += len;
            }
        }
        extension.clear();
        return true;
    };
    const auto compressed = std::filesystem::file_size(path);
    bool ok = stream_gzip(
        input,
        [&](std::string_view block) {
            // Tar padding and headers are not file payload. Bound framing separately so
            // empty archives still reach the empty-input check and endless padding stops.
            const auto framing = std::min<std::uint64_t>(limits.entry_count, 200000) * 2048 + 10240;
            if (position > UINT64_MAX - block.size() ||
                (position + block.size() > limits.total_bytes &&
                 position + block.size() - limits.total_bytes > framing))
                return fail("total byte limit (tar framing)");
            while (!block.empty()) {
                if (done) {
                    position += block.size();
                    break;
                }
                if (remain) {
                    auto n =
                        static_cast<std::size_t>(std::min<std::uint64_t>(remain, block.size()));
                    if (type == 'L' || type == 'x')
                        extension.append(block.substr(0, n));
                    block.remove_prefix(n);
                    position += n;
                    remain -= n;
                    if (!remain && !metadata())
                        return false;
                    continue;
                }
                if (padding) {
                    auto n =
                        static_cast<std::size_t>(std::min<std::uint64_t>(padding, block.size()));
                    block.remove_prefix(n);
                    position += n;
                    padding -= n;
                    continue;
                }
                auto n = std::min<std::size_t>(512 - header.size(), block.size());
                header.append(block.substr(0, n));
                block.remove_prefix(n);
                position += n;
                if (header.size() != 512)
                    continue;
                if (std::all_of(header.begin(), header.end(), [](char c) { return c == 0; })) {
                    if (++zeroes == 2)
                        done = true;
                    header.clear();
                    continue;
                }
                if (zeroes)
                    return fail("invalid tar end blocks");
                std::uint64_t checksum = 0, size = 0, sum = 0;
                if (!octal(std::string_view(header).substr(148, 8), checksum) ||
                    !octal(std::string_view(header).substr(124, 12), size))
                    return fail("invalid tar checksum or size");
                for (std::size_t i = 0; i < 512; ++i)
                    sum += (i >= 148 && i < 156) ? 32 : static_cast<unsigned char>(header[i]);
                if (sum != checksum)
                    return fail("tar checksum mismatch");
                if (++count > limits.entry_count)
                    return fail("entry count limit");
                type = header[156];
                std::string name = field(std::string_view(header).substr(0, 100));
                if (header.compare(257, 5, "ustar") == 0 && header[262] == '\0') {
                    auto prefix = field(std::string_view(header).substr(345, 155));
                    if (!prefix.empty())
                        name = prefix + "/" + name;
                }
                if (type != 'L' && type != 'x') {
                    if (!long_name.empty())
                        name = long_name;
                    if (!pax_path.empty())
                        name = pax_path;
                    if (has_pax_size)
                        size = pax_size;
                    long_name.clear();
                    pax_path.clear();
                    has_pax_size = false;
                    if (!canonical_path(name, error))
                        return false;
                    if (!seen.insert(name).second)
                        return fail("duplicate archive entry " + name);
                    if (size > limits.file_bytes)
                        return fail("file byte limit");
                    if (size > limits.total_bytes - total)
                        return fail("total byte limit");
                    total += size;
                    // Gzip has one shared compressed stream, without entry sizes.
                    // Its full size bounds each entry's compressed contribution.
                    if (static_cast<double>(size) > static_cast<double>(compressed) * limits.ratio)
                        return fail("ratio limit");
                    if (type == '0' || type == 0 || type == '5') {
                        entries.push_back({name, size, type == '5'});
                        offsets.push_back(position);
                    } else
                        unsupported.push_back({"IMPORT_UNSUPPORTED_ELEMENT", name, 0});
                } else if (size > 1024 * 1024 || size > limits.file_bytes)
                    return fail("file metadata limit");
                remain = size;
                padding = (512 - size % 512) % 512;
                header.clear();
            }
            return true;
        },
        error);
    if (!ok)
        return false;
    if (!done || remain || padding || !header.empty())
        return fail("truncated tar");
    return true;
}
} // namespace chipcae::geometry::ecad::detail
