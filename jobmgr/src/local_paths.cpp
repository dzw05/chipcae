#include "chipcae/jobmgr/local_paths.hpp"

#include "sha256_stream.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <limits>
#include <random>
#include <stdexcept>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace chipcae::jobmgr {
bool is_uuid(std::string_view id) {
    if (id.size() == 38 && id.front() == '{' && id.back() == '}')
        id = id.substr(1, 36);
    if (id.size() != 36)
        return false;
    bool nonzero = false;
    for (std::size_t i = 0; i < id.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (id[i] != '-')
                return false;
        } else {
            const char c = id[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
                return false;
            nonzero = nonzero || c != '0';
        }
    }
    return nonzero;
}
std::string make_run_id() {
    std::random_device random;
    std::uniform_int_distribution<unsigned> distribution(0, 255);
    std::array<unsigned, 16> bytes{};
    for (auto& byte : bytes)
        byte = distribution(random);
    bytes[6] = (bytes[6] & 0x0fU) | 0x40U;
    bytes[8] = (bytes[8] & 0x3fU) | 0x80U;
    constexpr char hex[] = "0123456789abcdef";
    std::string id;
    id.reserve(36);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10)
            id += '-';
        id += hex[bytes[i] >> 4];
        id += hex[bytes[i] & 0x0fU];
    }
    return id;
}
void check_snapshot_ref(std::string_view ref) {
    bool unsafe = ref.starts_with('/') || ref.find('\\') != std::string_view::npos ||
                  ref.find(':') != std::string_view::npos ||
                  ref.find('\0') != std::string_view::npos;
    for (std::size_t begin = 0; begin < ref.size();) {
        const auto end = ref.find('/', begin);
        if (ref.substr(begin, end == std::string_view::npos ? end : end - begin) == "..")
            unsafe = true;
        if (end == std::string_view::npos)
            break;
        begin = end + 1;
    }
    if (unsafe)
        throw std::runtime_error("Unsafe snapshot reference");
}
void write_file_atomic(const std::filesystem::path& path, std::string_view bytes) {
    auto parent = path.parent_path();
    if (parent.empty())
        parent = ".";
    std::filesystem::create_directories(parent);
    const auto pattern = (parent / (path.filename().string() + ".tmp-XXXXXX")).string();
    std::vector<char> temporary(pattern.begin(), pattern.end());
    temporary.push_back('\0');
    int fd = ::mkstemp(temporary.data());
    if (fd < 0)
        throw std::system_error(errno, std::generic_category(), "Create atomic file");
    try {
        while (!bytes.empty()) {
            const auto count = std::min(
                bytes.size(), static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
            const auto written = ::write(fd, bytes.data(), count);
            if (written < 0 && errno == EINTR)
                continue;
            if (written <= 0)
                throw std::system_error(
                    written < 0 ? errno : EIO, std::generic_category(), "Write atomic file");
            bytes.remove_prefix(static_cast<std::size_t>(written));
        }
        if (::fsync(fd) != 0)
            throw std::system_error(errno, std::generic_category(), "Sync atomic file");
        const int closing = fd;
        fd = -1;
        if (::close(closing) != 0)
            throw std::system_error(errno, std::generic_category(), "Close atomic file");
        if (::rename(temporary.data(), path.c_str()) != 0)
            throw std::system_error(errno, std::generic_category(), "Rename atomic file");
    } catch (...) {
        if (fd >= 0)
            ::close(fd);
        ::unlink(temporary.data());
        throw;
    }
}
} // namespace chipcae::jobmgr

// TEST-FIRST compile-only stub. The existing snapshot and atomic writer do not call it.
namespace chipcae::jobmgr::detail {
StreamedFile
stream_copy_hash(const std::filesystem::path&, const std::filesystem::path&, const std::string&) {
    return {};
}
} // namespace chipcae::jobmgr::detail
