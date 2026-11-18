#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace chipcae::core {

// String inputs are UTF-8; bytes are preserved through JSON escaping.
struct CrashError {
    std::string category{"FAILURE_CATEGORY_INTERNAL"};
    std::string code;
    std::uint32_t error_code{};
    std::string module;
    std::string message;
};

// Process-wide diagnostics. Serialize lifecycle, set_window and on_error calls
// in ordinary code; fatal signals may interrupt them on any thread.
class CrashDiag {
public:
    // Existing nonempty signal evidence is preserved by refusing installation.
    static void install(const std::filesystem::path& run_dir, int rank);
    // Nonfinite time throws invalid_argument without changing the last snapshot.
    static void set_window(std::uint64_t window, double time);
    static void on_error(const CrashError& error);
    static void on_signal(int sig);
    static void finalize();
    static bool installed() noexcept;
};

} // namespace chipcae::core
