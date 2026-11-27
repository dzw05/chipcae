#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace chipcae::jobmgr::detail {
inline constexpr std::size_t snapshot_chunk_bytes = std::size_t{1} << 20;

class Sha256 {
public:
    Sha256() noexcept;
    void update(const void* data, std::size_t size) noexcept;
    void update(std::string_view bytes) noexcept { update(bytes.data(), bytes.size()); }
    std::string hex_final();

private:
    std::array<std::uint32_t, 8> state_;
    std::array<unsigned char, 64> block_{};
    std::size_t block_used_ = 0;
    std::uint64_t total_bytes_ = 0;
};

struct StreamedFile {
    std::string sha256;
    std::uintmax_t bytes = 0;
    bool json_candidate = false;
};

StreamedFile stream_copy_hash(const std::filesystem::path& source,
                              const std::filesystem::path& target,
                              const std::string& expected_lowercase);
} // namespace chipcae::jobmgr::detail
