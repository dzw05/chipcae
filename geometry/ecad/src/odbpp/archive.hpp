#pragma once
#include "chipcae/geometry/ecad/odbpp.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <istream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
namespace chipcae::geometry::ecad::detail {
struct ArchiveEntry {
    std::string path;
    std::uint64_t size = 0;
    bool is_dir = false;
};
struct ArchiveLimits;
class ArchiveReader {
    struct State;
    std::shared_ptr<State> state_;
    enum class ReadStatus { success, unsupported, error };
    ReadStatus stream_bytes(const std::string&,
                            const std::function<bool(std::string_view)>&,
                            std::string&) const;

public:
    explicit ArchiveReader(const std::filesystem::path& input); // 目录、.tgz/.tar.gz、.zip
    ArchiveReader(const std::filesystem::path& input, const ArchiveLimits& limits);
    const std::vector<chipcae::geometry::ecad::UnsupportedElement>& unsupported() const;
    const std::string& open_error() const; // 空串表示成功，否则以 "IMPORT_REJECTED:" 开头
    const std::vector<ArchiveEntry>& entries() const; // ODB++ 内的相对路径，按出现顺序
    bool is_supported(const std::string& rel) const;
    bool read_bytes(const std::string& rel, std::string& out, std::string& err) const;
    bool for_each_line(const std::string& rel,
                       const std::function<bool(std::string_view)>& cb,
                       std::string& err) const; // cb 返回 false 时停止
};
} // namespace chipcae::geometry::ecad::detail

// Internal parser and codec interfaces; public import signatures remain unchanged.
#include "odb_job.hpp"
namespace chipcae::geometry::ecad::detail {
struct ArchiveLimits {
    std::uint64_t total_bytes = 2ULL * 1024 * 1024 * 1024;
    std::uint64_t file_bytes = 512ULL * 1024 * 1024;
    std::uint64_t entry_count = 200000;
    double ratio = 200;
};
bool decode_gzip(std::string_view input, std::string& output, std::string& error);
bool decode_lzw(std::string_view input, std::string& output, std::string& error);
ImportResult read_odbpp_job(const std::filesystem::path&, OdbJob&);
} // namespace chipcae::geometry::ecad::detail

namespace chipcae::geometry::ecad::detail {
using ByteSink = std::function<bool(std::string_view)>;
class GzipCursor {
    struct State;
    std::string error_;
    std::uint64_t consumed_ = 0;
    std::size_t output_limit_ = 32768;
    std::unique_ptr<State> state_;

public:
    explicit GzipCursor(std::istream&);
    ~GzipCursor();
    bool next(std::string_view&, std::string&, std::size_t max_output = 32768);
    std::uint64_t consumed_bytes() const { return consumed_; }
};
bool stream_gzip(std::istream&, const ByteSink&, std::string&);
bool index_tar(const std::filesystem::path&,
               const ArchiveLimits&,
               std::vector<ArchiveEntry>&,
               std::vector<std::uint64_t>&,
               std::vector<UnsupportedElement>&,
               std::string&);
bool parse_matrix(const ArchiveReader&, OdbJob&, std::string&);
bool parse_misc(const ArchiveReader&, OdbJob&, std::string&);
bool parse_features(const ArchiveReader&, OdbJob&, ImportResult&);
std::string layer_directory(const ArchiveReader&, const OdbJob&, const std::string&);
std::string trim(std::string_view);
std::string lower(std::string);
bool canonical_path(std::string&, std::string&);
} // namespace chipcae::geometry::ecad::detail
