#include "chipcae/geometry/import_accept.hpp"

// TEST-FIRST stubs: deliberately provide no accept-file behavior.
namespace chipcae::geometry {
std::filesystem::path default_accept_file(const std::filesystem::path&) {
    return {};
}
std::optional<AcceptList>
read_accept_file(const std::filesystem::path&, std::string*, std::vector<std::string>*) {
    return std::nullopt;
}
std::vector<std::string> accepted_ids(const AcceptList&) {
    return {};
}
} // namespace chipcae::geometry
