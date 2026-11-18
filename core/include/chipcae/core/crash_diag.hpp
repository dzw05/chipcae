#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace chipcae::core {

struct CrashError {
    std::string category{"FAILURE_CATEGORY_INTERNAL"};
    std::string code;
    std::uint32_t error_code{};
    std::string module;
    std::string message;
};

// TEST-FIRST STUB ONLY: no runtime behavior. Replace these empty definitions in
// the implementation pass; acceptance tests deliberately fail against this stub.
class CrashDiag {
public:
    static void install(const std::filesystem::path&, int) {}
    static void set_window(std::uint64_t, double) {}
    static void on_error(const CrashError&) {}
    static void on_signal(int) {}
    static void finalize() {}
    static bool installed() noexcept { return false; }
};

} // namespace chipcae::core
