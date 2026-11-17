#pragma once

#include <cstdint>
#include <string_view>

namespace chipcae {
/// Published failures follow protocol/errors.yaml. Zero means no error and is
/// not a protocol failure entry.
class ErrorCode {
public:
    enum Value : std::uint32_t {
        NONE = 0,
        INTERNAL = 1,
        NOT_IMPLEMENTED = 2,
        COLUMN_SPLIT_ACROSS_RANKS = 3,
        HIERARCHY_INVALID = 4,
        VERSION_UNSUPPORTED = 5,
        IMPORT_CONFIRMATION_REQUIRED = 6,
        IMPORT_REJECTED = 7,
        IMPORT_UNSUPPORTED_ELEMENT = 8,
        MATERIAL_MAPPING_MISSING = 9,
        INPUT_HASH_MISMATCH = 10,
        MATERIAL_ID_DUPLICATE = 11,
        INPUT_INVALID = 14,
        INPUT_REFERENCE_MISSING = 12,
        CUT_UNSUPPORTED = 13
    };
    constexpr ErrorCode(Value value = NONE) noexcept : value_(value) {}
    constexpr std::uint32_t code() const noexcept { return static_cast<std::uint32_t>(value_); }
    constexpr std::string_view name() const noexcept {
        switch (value_) {
        case NONE:
            return "NONE";
        case INTERNAL:
            return "INTERNAL";
        case NOT_IMPLEMENTED:
            return "NOT_IMPLEMENTED";
        case COLUMN_SPLIT_ACROSS_RANKS:
            return "COLUMN_SPLIT_ACROSS_RANKS";
        case HIERARCHY_INVALID:
            return "HIERARCHY_INVALID";
        case VERSION_UNSUPPORTED:
            return "VERSION_UNSUPPORTED";
        case IMPORT_CONFIRMATION_REQUIRED:
            return "IMPORT_CONFIRMATION_REQUIRED";
        case IMPORT_REJECTED:
            return "IMPORT_REJECTED";
        case IMPORT_UNSUPPORTED_ELEMENT:
            return "IMPORT_UNSUPPORTED_ELEMENT";
        case MATERIAL_MAPPING_MISSING:
            return "MATERIAL_MAPPING_MISSING";
        case INPUT_HASH_MISMATCH:
            return "INPUT_HASH_MISMATCH";
        case MATERIAL_ID_DUPLICATE:
            return "MATERIAL_ID_DUPLICATE";
        case INPUT_INVALID:
            return "INPUT_INVALID";
        case INPUT_REFERENCE_MISSING:
            return "INPUT_REFERENCE_MISSING";
        case CUT_UNSUPPORTED:
            return "CUT_UNSUPPORTED";
        }
        return "UNKNOWN";
    }

private:
    Value value_;
};
} // namespace chipcae
