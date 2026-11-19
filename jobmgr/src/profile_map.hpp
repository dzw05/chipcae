#pragma once

#include "chipcae/jobmgr/local_job_manager.hpp"

#include <jsoncons/json.hpp>

namespace chipcae::jobmgr::detail {
jobs::RunProfile map_profile(const jsoncons::json& profile, bool cuda_available);
LaunchCommand profile_command(const jsoncons::json& profile,
                              const LocalConfig& config,
                              const std::filesystem::path& case_path,
                              const std::filesystem::path& profile_path,
                              const std::filesystem::path& run_dir);
} // namespace chipcae::jobmgr::detail
