#include "archive_codecs.hpp"
namespace chipcae::geometry::ecad::detail {
// Explicitly selected brief fallback: callers report the source path as unsupported.
bool decode_lzw(std::string_view, std::string&, std::string& error) {
    error = "IMPORT_UNSUPPORTED_ELEMENT: .Z compression";
    return false;
}
} // namespace chipcae::geometry::ecad::detail
