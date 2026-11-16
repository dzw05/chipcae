#pragma once

#include <string_view>

namespace chipcae {

enum class LogLevel { debug, info, warn, error };

void set_log_level(LogLevel level);
LogLevel log_level() noexcept;
void log(LogLevel level, std::string_view message);

} // namespace chipcae
