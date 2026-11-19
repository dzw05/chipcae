#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace chipcae::jobmgr {
bool is_uuid(std::string_view id);
std::string make_run_id();
void check_snapshot_ref(std::string_view ref);
std::string sha256_hex(std::string_view bytes);
void write_file_atomic(const std::filesystem::path& path, std::string_view bytes);
} // namespace chipcae::jobmgr
