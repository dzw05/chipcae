#pragma once

#include <cstdint>
#include <string_view>

namespace chipcae {
/// Published failures follow protocol/errors.yaml. Zero means no error and is
/// not a protocol failure entry.
class ErrorCode {
public:
    enum Value : std::uint32_t { NONE = 0, INTERNAL = 1, NOT_IMPLEMENTED = 2 };
    constexpr ErrorCode(Value value = NONE) noexcept : value_(value) {}
    constexpr std::uint32_t code() const noexcept { return static_cast<std::uint32_t>(value_); }
    constexpr std::string_view name() const noexcept {
        return value_ == INTERNAL          ? "INTERNAL"
               : value_ == NOT_IMPLEMENTED ? "NOT_IMPLEMENTED"
                                           : "NONE";
    }

private:
    Value value_;
};
} // namespace chipcae
