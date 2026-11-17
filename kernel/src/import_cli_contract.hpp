#pragma once

// Process exit codes shared by the CLI entrypoint and format runners.
namespace chipcae::importer {
inline constexpr int exit_success = 0;
inline constexpr int exit_usage = 2;
inline constexpr int exit_io = 6;
inline constexpr int exit_import_rejected = 7;
inline constexpr int exit_material_mapping_missing = 9;
} // namespace chipcae::importer
