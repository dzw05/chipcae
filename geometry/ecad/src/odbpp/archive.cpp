#include "archive.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <miniz.h>
#include <set>

namespace chipcae::geometry::ecad::detail {
std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return s;
}
std::string trim(std::string_view s) {
    auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos)
        return {};
    return std::string(s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1));
}
bool canonical_path(std::string& path, std::string& error) {
    std::replace(path.begin(), path.end(), '\\', '/');
    while (path.starts_with("./"))
        path.erase(0, 2);
    bool invalid = path.empty() || path.front() == '/' || path.find(':') != std::string::npos ||
                   path.find('\0') != std::string::npos;
    std::string out;
    std::size_t i = 0;
    while (i < path.size()) {
        auto j = path.find('/', i);
        if (j == std::string::npos)
            j = path.size();
        auto part = path.substr(i, j - i);
        if (part == "..")
            invalid = true;
        if (!part.empty() && part != ".") {
            if (!out.empty())
                out += '/';
            out += part;
        }
        i = j + 1;
    }
    if (invalid || out.empty()) {
        error = "IMPORT_REJECTED: invalid archive path " + path;
        return false;
    }
    path = std::move(out);
    return true;
}
struct ArchiveReader::State {
    std::filesystem::path input;
    ArchiveLimits limits;
    std::vector<std::uint64_t> actual_sizes;
    std::uint64_t actual_total = 0;
    std::string error;
    std::vector<ArchiveEntry> entries;
    std::vector<std::uint64_t> offsets;
    std::vector<std::filesystem::path> directory_paths;
    std::vector<mz_uint> zip_indices;
    std::vector<bool> zip_supported;
    std::vector<UnsupportedElement> unsupported;
    mz_zip_archive zip{};
    bool zipped = false, directory = false;
    ~State() {
        if (zipped)
            mz_zip_reader_end(&zip);
    }
};
ArchiveReader::ArchiveReader(const std::filesystem::path& input)
    : ArchiveReader(input, ArchiveLimits{}) {}
ArchiveReader::ArchiveReader(const std::filesystem::path& input, const ArchiveLimits& limits)
    : state_(std::make_shared<State>()) {
    auto& s = *state_;
    s.input = input;
    s.limits = limits;
    auto fail = [&](std::string message) { s.error = "IMPORT_REJECTED: " + message; };
    try {
        std::uint64_t total = 0, count = 0;
        auto account = [&](std::uint64_t size) {
            if (++count > limits.entry_count) {
                fail("entry count limit");
                return false;
            }
            if (size > limits.file_bytes) {
                fail("file byte limit");
                return false;
            }
            if (size > limits.total_bytes - total) {
                fail("total byte limit");
                return false;
            }
            total += size;
            return true;
        };
        if (std::filesystem::is_directory(input)) {
            s.directory = true;
            for (const auto& e : std::filesystem::recursive_directory_iterator(input)) {
                auto rel = e.path().lexically_relative(input).generic_string();
                if (!canonical_path(rel, s.error))
                    return;
                if (e.is_symlink()) {
                    s.unsupported.push_back({"IMPORT_UNSUPPORTED_ELEMENT", rel, 0});
                    continue;
                }
                bool dir = e.is_directory();
                if (!dir && !e.is_regular_file()) {
                    s.unsupported.push_back({"IMPORT_UNSUPPORTED_ELEMENT", rel, 0});
                    continue;
                }
                auto size = dir ? 0 : e.file_size();
                if (!dir && !account(size))
                    return;
                s.entries.push_back({rel, size, dir});
                s.directory_paths.push_back(e.path());
            }
        } else {
            auto name = lower(input.filename().string());
            if (!std::filesystem::exists(input)) {
                fail("cannot open input");
                return;
            }
            if (std::filesystem::file_size(input) == 0) {
                fail("empty input");
                return;
            }
            if (name.ends_with(".z")) {
                s.unsupported.push_back(
                    {"IMPORT_UNSUPPORTED_ELEMENT", input.string() + ": .Z compression", 0});
                fail("unsupported .Z compression");
                return;
            }
            if (name.ends_with(".tgz") || name.ends_with(".tar.gz")) {
                if (!index_tar(input, limits, s.entries, s.offsets, s.unsupported, s.error))
                    return;
            } else if (name.ends_with(".zip")) {
                if (!mz_zip_reader_init_file(&s.zip, input.string().c_str(), 0)) {
                    fail("invalid or truncated zip");
                    return;
                }
                s.zipped = true;
                const bool zip64 = mz_zip_is_zip64(&s.zip) != 0;
                std::ifstream local(input, std::ios::binary);
                const auto archive_size = std::filesystem::file_size(input);
                if (zip64)
                    s.unsupported.push_back(
                        {"IMPORT_UNSUPPORTED_ELEMENT", input.string() + ": zip64", 0});
                std::set<std::string> seen;
                for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&s.zip); ++i) {
                    mz_zip_archive_file_stat st{};
                    if (!mz_zip_reader_file_stat(&s.zip, i, &st)) {
                        fail("invalid zip entry");
                        return;
                    }
                    auto length = mz_zip_reader_get_filename(&s.zip, i, nullptr, 0);
                    std::string rel(length, '\0');
                    if (!length || !mz_zip_reader_get_filename(&s.zip, i, rel.data(), length)) {
                        fail("invalid zip path");
                        return;
                    }
                    rel.pop_back();
                    if (rel.find('\0') != std::string::npos || !canonical_path(rel, s.error)) {
                        if (s.error.empty())
                            fail("invalid zip path");
                        return;
                    }
                    if (!seen.insert(rel).second) {
                        fail("duplicate archive entry " + rel);
                        return;
                    }
                    if (!account(st.m_uncomp_size))
                        return;
                    if (static_cast<double>(st.m_uncomp_size) >
                        static_cast<double>(st.m_comp_size) * limits.ratio) {
                        fail("ratio limit");
                        return;
                    }
                    // Local-only zip64 headers need not set miniz's central-directory flag.
                    if (archive_size < 30 || st.m_local_header_ofs > archive_size - 30) {
                        fail("truncated zip local header");
                        return;
                    }
                    local.seekg(static_cast<std::streamoff>(st.m_local_header_ofs));
                    std::array<unsigned char, 30> header{};
                    local.read(reinterpret_cast<char*>(header.data()), header.size());
                    if (!local || header[0] != 'P' || header[1] != 'K' || header[2] != 3 ||
                        header[3] != 4) {
                        fail("invalid zip local header");
                        return;
                    }
                    auto u16 = [&](std::size_t offset) {
                        return static_cast<unsigned>(header[offset]) + 256U * header[offset + 1];
                    };
                    auto name_size = u16(26), extra_size = u16(28);
                    if (name_size + extra_size > archive_size - st.m_local_header_ofs - 30) {
                        fail("truncated zip extra field");
                        return;
                    }
                    local.seekg(static_cast<std::streamoff>(name_size), std::ios::cur);
                    std::string extra(extra_size, '\0');
                    local.read(extra.data(), extra_size);
                    bool local_zip64 = false;
                    for (std::size_t pos = 0; pos < extra.size();) {
                        if (extra.size() - pos < 4) {
                            fail("invalid zip extra field");
                            return;
                        }
                        auto word = [&](std::size_t off) {
                            return static_cast<unsigned char>(extra[off]) +
                                   256U * static_cast<unsigned char>(extra[off + 1]);
                        };
                        auto id = word(pos), size = word(pos + 2);
                        if (size > extra.size() - pos - 4) {
                            fail("invalid zip extra field");
                            return;
                        }
                        if (id == 1)
                            local_zip64 = true;
                        pos += 4 + size;
                    }
                    bool supported = !zip64 && !local_zip64 && !st.m_is_encrypted &&
                                     (st.m_method == 0 || st.m_method == 8);
                    if (!supported)
                        s.unsupported.push_back({"IMPORT_UNSUPPORTED_ELEMENT",
                                                 rel + ": zip compression/encryption/zip64",
                                                 0});
                    s.entries.push_back({rel, st.m_uncomp_size, st.m_is_directory != 0});
                    s.zip_indices.push_back(i);
                    s.zip_supported.push_back(supported);
                }
            } else {
                fail("unsupported archive extension");
                return;
            }
        }
        for (const auto& entry : s.entries) {
            if (!entry.is_dir && lower(entry.path).ends_with(".z"))
                s.unsupported.push_back(
                    {"IMPORT_UNSUPPORTED_ELEMENT", entry.path + ": .Z compression", 0});
        }
        if (std::none_of(
                s.entries.begin(), s.entries.end(), [](const auto& e) { return !e.is_dir; }))
            fail("empty input");
    } catch (const std::exception& e) {
        fail(e.what());
    }
}
const std::string& ArchiveReader::open_error() const {
    return state_->error;
}
const std::vector<ArchiveEntry>& ArchiveReader::entries() const {
    return state_->entries;
}
const std::vector<UnsupportedElement>& ArchiveReader::unsupported() const {
    return state_->unsupported;
}
namespace {
struct ZipSink {
    const ByteSink& sink;
    static std::size_t write(void* opaque, mz_uint64, const void* bytes, std::size_t size) {
        return static_cast<ZipSink*>(opaque)->sink({static_cast<const char*>(bytes), size}) ? size
                                                                                            : 0;
    }
};
} // namespace
// Enforce limits against delivered bytes as well as archive metadata. Repeated reads
// count each entry's largest observed size once toward the archive total.
bool ArchiveReader::stream_bytes(const std::string& rel,
                                 const ByteSink& consume,
                                 std::string& error) const {
    auto& s = *state_;
    error.clear();
    if (!s.error.empty()) {
        error = s.error;
        return false;
    }
    auto it = std::find_if(s.entries.begin(), s.entries.end(), [&](const auto& e) {
        return e.path == rel && !e.is_dir;
    });
    if (it == s.entries.end()) {
        error = "IMPORT_REJECTED: missing file " + rel;
        return false;
    }
    auto index = static_cast<std::size_t>(it - s.entries.begin());
    s.actual_sizes.resize(s.entries.size());
    std::uint64_t delivered = 0;
    ByteSink sink = [&](std::string_view b) {
        if (b.size() > s.limits.file_bytes - delivered) {
            error = "IMPORT_REJECTED: file byte limit " + rel;
            return false;
        }
        delivered += b.size();
        auto growth = delivered > s.actual_sizes[index] ? delivered - s.actual_sizes[index] : 0;
        if (growth > s.limits.total_bytes - s.actual_total) {
            error = "IMPORT_REJECTED: total byte limit " + rel;
            return false;
        }
        s.actual_total += growth;
        s.actual_sizes[index] += growth;
        return consume(b);
    };
    if (s.directory) {
        std::ifstream in(s.directory_paths[index], std::ios::binary);
        if (!in) {
            error = "IMPORT_REJECTED: cannot read " + rel;
            return false;
        }
        std::array<char, 32768> buffer{};
        while (in) {
            in.read(buffer.data(), buffer.size());
            if (!sink({buffer.data(), static_cast<std::size_t>(in.gcount())}))
                return false;
        }
        if (in.bad()) {
            error = "IMPORT_REJECTED: read failure " + rel;
            return false;
        }
        return true;
    }
    if (s.zipped) {
        if (!s.zip_supported[index]) {
            error = "IMPORT_REJECTED: required file uses unsupported zip encoding " + rel;
            return false;
        }
        ZipSink adapter{sink};
        if (mz_zip_reader_extract_to_callback(
                &s.zip, s.zip_indices[index], ZipSink::write, &adapter, 0))
            return true;
        if (error.empty())
            error = "IMPORT_REJECTED: zip CRC or corrupt payload " + rel;
        return false;
    }
    std::ifstream in(s.input, std::ios::binary);
    std::uint64_t pos = 0, start = s.offsets[index], end = start + it->size;
    std::string codec_error;
    bool ok = stream_gzip(
        in,
        [&](std::string_view b) {
            auto finish = pos + b.size();
            if (pos < end && finish > start &&
                !sink(b.substr(
                    static_cast<std::size_t>(std::max(pos, start) - pos),
                    static_cast<std::size_t>(std::min(finish, end) - std::max(pos, start)))))
                return false;
            pos = finish;
            return true;
        },
        codec_error);
    if (!ok && error.empty())
        error = codec_error;
    return ok;
}
bool ArchiveReader::for_each_line(const std::string& rel,
                                  const std::function<bool(std::string_view)>& cb,
                                  std::string& error) const {
    constexpr std::size_t max_line_bytes = 1024 * 1024;
    std::string pending;
    bool stopped = false;
    const bool ok = stream_bytes(
        rel,
        [&](std::string_view block) {
            if (stopped)
                return true;
            while (!block.empty()) {
                auto end = block.find('\n');
                auto length = end == std::string_view::npos ? block.size() : end;
                if (length > max_line_bytes - pending.size()) {
                    error = "IMPORT_REJECTED: line byte limit " + rel;
                    return false;
                }
                if (end == std::string_view::npos) {
                    pending.append(block);
                    break;
                }
                if (pending.empty())
                    stopped = !cb(block.substr(0, end));
                else {
                    pending.append(block.substr(0, end));
                    stopped = !cb(pending);
                    pending.clear();
                }
                if (stopped)
                    break;
                block.remove_prefix(end + 1);
            }
            return true;
        },
        error);
    if (ok && !stopped && !pending.empty())
        cb(pending);
    return ok;
}
bool ArchiveReader::read_bytes(const std::string& rel, std::string& out, std::string& error) const {
    out.clear();
    return stream_bytes(
        rel,
        [&](std::string_view b) {
            out.append(b);
            return true;
        },
        error);
}
} // namespace chipcae::geometry::ecad::detail
