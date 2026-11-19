#include "launcher_lock.hpp"

#include <array>
#include <cerrno>
#include <charconv>
#include <fcntl.h>
#include <limits>
#include <signal.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace chipcae::jobmgr::detail {
namespace {
constexpr char locked[] = "Local job root already has a control writer";
struct ScopedFd {
    int value;
    explicit ScopedFd(int fd) : value(fd) {}
    ~ScopedFd() {
        if (value >= 0)
            ::close(value);
    }
    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;
};
bool same_file(int fd, const std::filesystem::path& path) {
    struct stat opened{}, current{};
    return ::fstat(fd, &opened) == 0 && ::lstat(path.c_str(), &current) == 0 &&
           opened.st_dev == current.st_dev && opened.st_ino == current.st_ino;
}
std::string hostname() {
    std::array<char, 256> host{};
    if (::gethostname(host.data(), host.size() - 1) != 0)
        throw std::runtime_error(locked);
    return host.data();
}
bool stale(int fd, const std::string& host) {
    std::array<char, 4096> bytes{};
    const auto count = ::pread(fd, bytes.data(), bytes.size(), 0);
    if (count <= 0)
        return false;
    std::istringstream input(std::string(bytes.data(), static_cast<std::size_t>(count)));
    std::string pid_text, app, owner_host;
    if (!std::getline(input, pid_text) || !std::getline(input, app) ||
        !std::getline(input, owner_host) || owner_host != host)
        return false;
    long pid = 0;
    const auto parsed = std::from_chars(pid_text.data(), pid_text.data() + pid_text.size(), pid);
    if (parsed.ec != std::errc{} || parsed.ptr != pid_text.data() + pid_text.size() || pid <= 0 ||
        pid > std::numeric_limits<pid_t>::max())
        return false;
    // A reused live PID, a remote host or an inaccessible process is never demonstrably stale.
    return ::kill(static_cast<pid_t>(pid), 0) == -1 && errno == ESRCH;
}
} // namespace
LauncherLock::LauncherLock(const std::filesystem::path& root) : path_(root / "launcher.lock") {
    std::filesystem::create_directories(root);
    const auto host = hostname();
    const auto content = std::to_string(::getpid()) + "\nchipcae-jobmgr\n" + host + '\n';
    for (int attempt = 0; attempt < 16; ++attempt) {
        const int created = ::open(path_.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
        if (created >= 0) {
            fd_ = created;
            bool ok = ::flock(fd_, LOCK_EX | LOCK_NB) == 0;
            std::size_t offset = 0;
            while (ok && offset < content.size()) {
                const auto written = ::write(fd_, content.data() + offset, content.size() - offset);
                if (written < 0 && errno == EINTR)
                    continue;
                if (written <= 0)
                    ok = false;
                else
                    offset += static_cast<std::size_t>(written);
            }
            if (ok)
                ok = ::fsync(fd_) == 0;
            if (ok)
                return;
            if (same_file(fd_, path_))
                ::unlink(path_.c_str());
            ::close(fd_);
            fd_ = -1;
            throw std::runtime_error(locked);
        }
        if (errno != EEXIST)
            throw std::runtime_error(locked);
        const ScopedFd existing(::open(path_.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW));
        if (existing.value < 0) {
            if (errno == ENOENT)
                continue;
            throw std::runtime_error(locked);
        }
        if (::flock(existing.value, LOCK_EX | LOCK_NB) != 0)
            throw std::runtime_error(locked);
        if (!same_file(existing.value, path_))
            continue;
        const bool dead = stale(existing.value, host);
        const bool removed = dead && ::unlink(path_.c_str()) == 0;
        if (!removed)
            throw std::runtime_error(locked);
    }
    throw std::runtime_error(locked);
}
LauncherLock::~LauncherLock() {
    if (fd_ >= 0) {
        if (same_file(fd_, path_))
            ::unlink(path_.c_str());
        ::close(fd_);
    }
}
} // namespace chipcae::jobmgr::detail
