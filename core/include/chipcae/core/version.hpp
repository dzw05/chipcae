#pragma once

#include <string_view>

namespace chipcae {

constexpr int version_major = 0;
constexpr int version_minor = 1;
constexpr int version_patch = 0;

constexpr std::string_view version_string() noexcept {
    return "0.1.0";
}

} // namespace chipcae
