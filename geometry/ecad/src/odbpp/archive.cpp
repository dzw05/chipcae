#include "archive_internal.hpp"

#include <algorithm>
#include <array>
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
bool canonical_path(std::string& path, std::string& error) {
    std::replace(path.begin(), path.end(), '\\', '/');
    while (path.starts_with("./"))
        path.erase(0, 2);
    bool invalid = path.empty() || path.front() == '/' ||
                   (path.size() >= 2 &&
                    ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
                    path[1] == ':') ||
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
    std::vector<std::string> zip_reasons;
    std::vector<UnsupportedElement> unsupported;
    mz_zip_archive zip{};
    std::ifstream tar_input;
    std::unique_ptr<GzipCursor> tar_cursor;
    std::string_view tar_block;
    std::uint64_t tar_position = 0;
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
        std::uint64_t count = 0;
        auto count_entry = [&]() {
            if (++count <= limits.entry_count)
                return true;
            fail("entry count limit");
            return false;
        };
        if (std::filesystem::is_directory(input)) {
            s.directory = true;
            std::error_code ec;
            const auto options = std::filesystem::directory_options::none;
            std::filesystem::recursive_directory_iterator it(input, options, ec), end;
            if (ec) {
                fail("cannot enumerate input: " + ec.message());
                return;
            }
            while (it != end) {
                const auto e = *it;
                if (!count_entry())
                    return;
                auto rel = e.path().lexically_relative(input).generic_string();
                if (!canonical_path(rel, s.error))
                    return;
                auto report = [&](const std::string& reason) {
                    s.unsupported.push_back({"IMPORT_UNSUPPORTED_ELEMENT", rel + ": " + reason, 0});
                };
                const auto status = e.symlink_status(ec);
                if (ec) {
                    fail("cannot inspect directory entry " + rel + ": " + ec.message());
                    return;
                } else if (std::filesystem::is_symlink(status)) {
                    fail("directory input symlink " + rel);
                    return;
                } else {
                    const bool dir = std::filesystem::is_directory(status);
                    if (!dir && !std::filesystem::is_regular_file(status)) {
                        report("unsupported directory entry type");
                    } else {
                        const auto size = dir ? 0 : e.file_size(ec);
                        if (ec) {
                            fail("cannot read file size " + rel + ": " + ec.message());
                            return;
                        } else {
                            s.entries.push_back({rel, size, dir});
                            s.directory_paths.push_back(e.path());
                        }
                        if (dir) {
                            std::filesystem::directory_iterator probe(e.path(), ec);
                            if (ec) {
                                fail("cannot read directory " + rel + ": " + ec.message());
                                return;
                            }
                        }
                    }
                }
                ec.clear();
                it.increment(ec);
                if (ec) {
                    fail("cannot traverse directory " + rel + ": " + ec.message());
                    return;
                }
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
            if (name.ends_with(".tgz") || name.ends_with(".tar.gz")) {
                if (!index_tar(input, limits, s.entries, s.offsets, s.unsupported, s.error))
                    return;
            } else if (name.ends_with(".zip")) {
                if (!mz_zip_reader_init_file(&s.zip, input.string().c_str(), 0)) {
                    fail("invalid or truncated zip");
                    return;
                }
                s.zipped = true;
                if (mz_zip_reader_get_num_files(&s.zip) > limits.entry_count) {
                    fail("entry count limit");
                    return;
                }
                std::ifstream local(input, std::ios::binary);
                if (!local) {
                    fail("cannot open zip local headers");
                    return;
                }
                const auto archive_size = std::filesystem::file_size(input);
                std::set<std::string> seen;
                for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&s.zip); ++i) {
                    if (!count_entry())
                        return;
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
                    // Local-only zip64 headers need not set miniz's central-directory flag.
                    if (archive_size < 30 || st.m_local_header_ofs > archive_size - 30) {
                        fail("truncated zip local header");
                        return;
                    }
                    local.clear();
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
                    // Inspect both headers per entry; the archive-wide ZIP64 marker
                    // does not make ordinary members unsupported.
                    bool entry_zip64 = false;
                    auto extra_fields =
                        [&](std::uint64_t offset, unsigned name_size, unsigned extra_size) {
                            if (offset > archive_size ||
                                name_size + extra_size > archive_size - offset) {
                                fail("truncated zip extra field");
                                return false;
                            }
                            local.clear();
                            local.seekg(static_cast<std::streamoff>(offset + name_size));
                            std::string extra(extra_size, '\0');
                            local.read(extra.data(), extra_size);
                            if (!local) {
                                fail("cannot read zip extra field");
                                return false;
                            }
                            for (std::size_t pos = 0; pos < extra.size();) {
                                if (extra.size() - pos < 4) {
                                    fail("invalid zip extra field");
                                    return false;
                                }
                                auto word = [&](std::size_t off) {
                                    return static_cast<unsigned char>(extra[off]) +
                                           256U * static_cast<unsigned char>(extra[off + 1]);
                                };
                                auto id = word(pos), size = word(pos + 2);
                                if (size > extra.size() - pos - 4) {
                                    fail("invalid zip extra field");
                                    return false;
                                }
                                entry_zip64 = entry_zip64 || id == 1;
                                pos += 4 + size;
                            }
                            return true;
                        };
                    auto sentinel = [](const auto& bytes, std::size_t pos) {
                        return std::all_of(bytes.begin() + pos,
                                           bytes.begin() + pos + 4,
                                           [](unsigned char c) { return c == 255; });
                    };
                    entry_zip64 = sentinel(header, 18) || sentinel(header, 22);
                    if (!extra_fields(st.m_local_header_ofs + 30, u16(26), u16(28)))
                        return;
                    const auto central_offset =
                        s.zip.m_central_directory_file_ofs + st.m_central_dir_ofs;
                    if (archive_size < 46 || central_offset > archive_size - 46) {
                        fail("truncated zip central header");
                        return;
                    }
                    std::array<unsigned char, 46> central{};
                    local.clear();
                    local.seekg(static_cast<std::streamoff>(central_offset));
                    local.read(reinterpret_cast<char*>(central.data()), central.size());
                    if (!local || central[0] != 'P' || central[1] != 'K' || central[2] != 1 ||
                        central[3] != 2) {
                        fail("invalid zip central header");
                        return;
                    }
                    entry_zip64 = entry_zip64 || sentinel(central, 20) || sentinel(central, 24) ||
                                  sentinel(central, 42) ||
                                  (central[34] == 255 && central[35] == 255);
                    auto central_word = [&](std::size_t off) {
                        return static_cast<unsigned>(central[off]) + 256U * central[off + 1];
                    };
                    if (!extra_fields(central_offset + 46, central_word(28), central_word(30)))
                        return;
                    bool supported = !entry_zip64 && !st.m_is_encrypted &&
                                     (st.m_method == 0 || st.m_method == 8);
                    if (supported && !st.m_is_directory) {
                        if (st.m_comp_size == 0 && st.m_uncomp_size != 0) {
                            fail("corrupt zip entry: zero compressed size " + rel);
                            return;
                        }
                    }
                    const std::string reason = entry_zip64         ? "zip64"
                                               : st.m_is_encrypted ? "zip encryption"
                                               : supported         ? ""
                                                                   : "zip compression method";
                    if (!supported)
                        s.unsupported.push_back(
                            {"IMPORT_UNSUPPORTED_ELEMENT", rel + ": " + reason, 0});
                    s.zip_reasons.push_back(reason);
                    s.entries.push_back(
                        {rel, st.m_uncomp_size, st.m_is_directory != 0, st.m_comp_size});
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
        auto block = std::string_view(static_cast<const char*>(bytes), size);
        while (!block.empty()) {
            auto chunk = block.substr(0, 32768);
            if (!static_cast<ZipSink*>(opaque)->sink(chunk))
                return 0;
            block.remove_prefix(chunk.size());
        }
        return size;
    }
};
} // namespace
bool ArchiveReader::is_supported(const std::string& rel) const {
    const auto& s = *state_;
    for (std::size_t i = 0; i < s.entries.size(); ++i)
        if (s.entries[i].path == rel)
            return !lower(rel).ends_with(".z") && (!s.zipped || s.zip_supported[i]);
    return true;
}
// Enforce delivered-byte limits too; repeated reads charge each entry only once.
ArchiveReader::ReadStatus ArchiveReader::stream_bytes(const std::string& rel,
                                                      const ByteSink& consume,
                                                      std::string& error) const {
    auto& s = *state_;
    error.clear();
    if (!s.error.empty()) {
        error = s.error;
        return ReadStatus::error;
    }
    auto it = std::find_if(s.entries.begin(), s.entries.end(), [&](const auto& e) {
        return e.path == rel && !e.is_dir;
    });
    if (it == s.entries.end()) {
        // Required ODB++ text may be present only as an unsupported compressed file.
        const auto compressed =
            std::find_if(s.entries.begin(), s.entries.end(), [&](const auto& e) {
                return !e.is_dir && (e.path == rel + ".Z" || e.path == rel + ".z");
            });
        if (compressed != s.entries.end()) {
            error = "IMPORT_REJECTED: required file uses unsupported .Z compression " +
                    compressed->path;
            return ReadStatus::unsupported;
        }
        error = "IMPORT_REJECTED: missing file " + rel;
        return ReadStatus::error;
    }
    auto index = static_cast<std::size_t>(it - s.entries.begin());
    if (!is_supported(rel)) {
        const auto reason = lower(rel).ends_with(".z") ? ".Z compression" : s.zip_reasons[index];
        error =
            "IMPORT_REJECTED: required file uses unsupported " + std::string(reason) + " " + rel;
        return ReadStatus::unsupported;
    }
    s.actual_sizes.resize(s.entries.size());
    std::uint64_t delivered = 0;
    ByteSink sink = [&](std::string_view b) {
        if (!s.directory && static_cast<double>(delivered + b.size()) >
                                static_cast<double>(it->compressed_size) * s.limits.ratio) {
            error = "IMPORT_REJECTED: ratio limit " + rel;
            return false;
        }
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
        // A consumer may stop within this block (for example after its first line).
        // Commit only accepted blocks so its unread tail cannot charge later reads.
        if (!consume(b))
            return false;
        s.actual_total += growth;
        s.actual_sizes[index] += growth;
        return true;
    };
    if (s.directory) {
        std::ifstream in(s.directory_paths[index], std::ios::binary);
        if (!in) {
            error = "IMPORT_REJECTED: cannot read " + rel;
            return ReadStatus::error;
        }
        std::array<char, 32768> buffer{};
        while (in) {
            in.read(buffer.data(), buffer.size());
            if (!sink({buffer.data(), static_cast<std::size_t>(in.gcount())}))
                return ReadStatus::error;
        }
        if (in.bad()) {
            error = "IMPORT_REJECTED: read failure " + rel;
            return ReadStatus::error;
        }
        return ReadStatus::success;
    }
    if (s.zipped) {
        ZipSink adapter{sink};
        if (mz_zip_reader_extract_to_callback(
                &s.zip, s.zip_indices[index], ZipSink::write, &adapter, 0))
            return ReadStatus::success;
        if (error.empty())
            error = "IMPORT_REJECTED: zip CRC or corrupt payload " + rel;
        return ReadStatus::error;
    }
    const auto start = s.offsets[index], end = start + it->size;
    if (!s.tar_cursor || start < s.tar_position) {
        s.tar_cursor.reset();
        s.tar_input.close();
        s.tar_input.clear();
        s.tar_input.open(s.input, std::ios::binary);
        s.tar_cursor = std::make_unique<GzipCursor>(s.tar_input, UINT64_MAX);
        s.tar_position = 0;
        s.tar_block = {};
    }
    while (s.tar_position < end) {
        if (s.tar_block.empty() && !s.tar_cursor->next(s.tar_block, error)) {
            if (error.empty())
                error = "IMPORT_REJECTED: truncated tar payload " + rel;
            return ReadStatus::error;
        }
        const auto finish = std::min<std::uint64_t>(s.tar_position + s.tar_block.size(), end);
        const auto length = static_cast<std::size_t>(finish - s.tar_position);
        const auto skip =
            static_cast<std::size_t>(std::min(finish, start) - std::min(s.tar_position, start));
        if (length > skip && !sink(s.tar_block.substr(skip, length - skip)))
            return ReadStatus::error;
        s.tar_block.remove_prefix(length);
        s.tar_position = finish;
    }
    return ReadStatus::success;
}
bool ArchiveReader::for_each_line(const std::string& rel,
                                  const std::function<bool(std::string_view)>& cb,
                                  std::string& error) const {
    constexpr std::size_t max_line_bytes = 1024 * 1024;
    std::string pending;
    bool stopped = false;
    std::string callback_error;
    const auto status = stream_bytes(
        rel,
        [&](std::string_view block) {
            while (!block.empty()) {
                auto end = block.find('\n');
                auto length = end == std::string_view::npos ? block.size() : end;
                // Keep one possible CR terminator across input block boundaries.
                if (length > max_line_bytes + 1 - pending.size()) {
                    error = "IMPORT_REJECTED: line byte limit " + rel;
                    return false;
                }
                pending.append(block.substr(0, length));
                if (pending.size() > max_line_bytes && pending.back() != '\r') {
                    error = "IMPORT_REJECTED: line byte limit " + rel;
                    return false;
                }
                if (end == std::string_view::npos)
                    break;
                if (!pending.empty() && pending.back() == '\r')
                    pending.pop_back();
                stopped = !cb(pending);
                pending.clear();
                if (stopped) {
                    callback_error = error;
                    return false;
                }
                block.remove_prefix(end + 1);
            }
            return true;
        },
        error);
    if (stopped) {
        error = callback_error;
        return error.empty();
    }
    const bool final_record = !pending.empty();
    if (!pending.empty() && pending.back() == '\r')
        pending.pop_back();
    if (status == ReadStatus::success && final_record && !cb(pending))
        return error.empty();
    return status == ReadStatus::success;
}
bool ArchiveReader::read_bytes(const std::string& rel, std::string& out, std::string& error) const {
    out.clear();
    return stream_bytes(
               rel,
               [&](std::string_view b) {
                   out.append(b);
                   return true;
               },
               error) == ReadStatus::success;
}
} // namespace chipcae::geometry::ecad::detail
