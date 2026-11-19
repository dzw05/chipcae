#include "chipcae/jobmgr/local_paths.hpp"

// TEST-FIRST linkage stubs only. Acceptance tests deliberately remain red.
namespace chipcae::jobmgr {
bool is_uuid(std::string_view) {
    return false;
}
std::string make_run_id() {
    return {};
}
void check_snapshot_ref(std::string_view) {}
std::string sha256_hex(std::string_view) {
    return {};
}
void write_file_atomic(const std::filesystem::path&, std::string_view) {}
} // namespace chipcae::jobmgr
