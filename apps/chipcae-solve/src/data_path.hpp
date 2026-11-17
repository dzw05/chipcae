#pragma once

#include <cstdlib>
#include <filesystem>
#include <vector>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

namespace chipcae {
inline std::filesystem::path executable_directory() {
#ifdef __APPLE__
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buffer(size);
    if (_NSGetExecutablePath(buffer.data(), &size) == 0)
        return std::filesystem::canonical(buffer.data()).parent_path();
#elif defined(__linux__)
    std::vector<char> buffer(1024);
    for (;;) {
        auto size = readlink("/proc/self/exe", buffer.data(), buffer.size());
        if (size < 0)
            break;
        if (static_cast<std::size_t>(size) < buffer.size())
            return std::filesystem::path(std::string(buffer.data(), size)).parent_path();
        buffer.resize(buffer.size() * 2);
    }
#endif
    return {};
}

// A nonempty override is authoritative: missing files retain existing loader errors.
inline std::filesystem::path data_path(const std::filesystem::path& source_relative) {
    const auto installed_relative = std::filesystem::path("schema") / source_relative.filename();
    if (const char* root = std::getenv("CHIPCAE_DATA_DIR"); root && *root)
        return std::filesystem::path(root) / installed_relative;
    const auto executable = executable_directory();
    const auto installed = executable / "../share/chipcae" / installed_relative;
    if (!executable.empty() && std::filesystem::is_regular_file(installed))
        return installed;
    return executable / CHIPCAE_SOURCE_RELATIVE / source_relative;
}
} // namespace chipcae
