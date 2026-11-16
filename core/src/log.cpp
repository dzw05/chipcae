#include "chipcae/core/log.hpp"

#include <iostream>
#include <mutex>
#include <string_view>

namespace chipcae {
namespace {

LogLevel g_level = LogLevel::info;
std::mutex g_mutex;

const char* to_string(LogLevel level) noexcept {
    switch (level) {
    case LogLevel::debug:
        return "debug";
    case LogLevel::info:
        return "info";
    case LogLevel::warn:
        return "warn";
    case LogLevel::error:
        return "error";
    }
    return "unknown";
}

} // namespace

void set_log_level(LogLevel level) {
    g_level = level;
}

LogLevel log_level() noexcept {
    return g_level;
}

void log(LogLevel level, std::string_view message) {
    if (static_cast<int>(level) < static_cast<int>(g_level)) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    std::cerr << "[chipcae:" << to_string(level) << "] " << message << '\n';
}

} // namespace chipcae
