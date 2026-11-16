#pragma once

#include <string_view>

namespace chipcae {
/// Published failures follow protocol/errors.yaml. Zero means no error and is
/// not a protocol failure entry.
class ErrorCode {
public:
    enum Value : int { NONE = 0, INTERNAL = 1 };
    constexpr ErrorCode(Value value = NONE) noexcept : value_(value) {}
    constexpr int code() const noexcept { return static_cast<int>(value_); }
    constexpr std::string_view name() const noexcept {
        return value_ == INTERNAL ? "INTERNAL" : "NONE";
    }

private:
    Value value_;
};
} // namespace chipcae
