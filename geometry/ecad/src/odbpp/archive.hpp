#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
namespace chipcae::geometry::ecad {
struct UnsupportedElement;
}
namespace chipcae::geometry::ecad::detail {
struct ArchiveEntry {
    std::string path;
    std::uint64_t size = 0;
    bool is_dir = false;
    std::uint64_t compressed_size = 0;
};
struct ArchiveLimits {
    std::uint64_t total_bytes = 2ULL * 1024 * 1024 * 1024;
    std::uint64_t file_bytes = 512ULL * 1024 * 1024;
    std::uint64_t entry_count = 200000;
    double ratio = 200;
};
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
    // Like std::getline: empty input and a terminal newline add no final record.
    bool for_each_line(const std::string& rel,
                       const std::function<bool(std::string_view)>& cb,
                       std::string& err) const; // cb 返回 false 时停止
};

} // namespace chipcae::geometry::ecad::detail
