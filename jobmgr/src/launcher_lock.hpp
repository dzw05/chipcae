#pragma once

#include <filesystem>

namespace chipcae::jobmgr::detail {
// Own both the exclusive-create lock file and a nonblocking advisory lock.
class LauncherLock {
public:
    explicit LauncherLock(const std::filesystem::path& root);
    ~LauncherLock();
    LauncherLock(const LauncherLock&) = delete;
    LauncherLock& operator=(const LauncherLock&) = delete;

private:
    std::filesystem::path path_;
    int fd_ = -1;
};
} // namespace chipcae::jobmgr::detail
