#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>
namespace chipcae::geometry::ecad::detail {
struct ArchiveEntry {
    std::string path;
    std::uint64_t size = 0;
    bool is_dir = false;
};
class ArchiveReader {
public:
    explicit ArchiveReader(const std::filesystem::path& input); // 目录、.tgz/.tar.gz、.zip
    const std::string& open_error() const; // 空串表示成功，否则以 "IMPORT_REJECTED:" 开头
    const std::vector<ArchiveEntry>& entries() const; // ODB++ 内的相对路径，按出现顺序
    bool read_bytes(const std::string& rel, std::string& out, std::string& err) const;
    bool for_each_line(const std::string& rel,
                       const std::function<bool(std::string_view)>& cb,
                       std::string& err) const; // cb 返回 false 时停止
};
} // namespace chipcae::geometry::ecad::detail
