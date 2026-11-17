#pragma once

#include <filesystem>
#include <optional>
#include <string>

// Private to chipcae-import; this header is not an installed public API.
namespace chipcae::importer {
struct ImportOptions {
    std::filesystem::path input, output, polyset, report, material_map;
    bool accept_b_tier = false;
    std::filesystem::path accept_file;
    std::optional<double> t_min;
    std::filesystem::path stack_override, assembly;
};

inline constexpr int exit_success = 0;
inline constexpr int exit_usage = 2;
inline constexpr int exit_io = 6;
inline constexpr int exit_import_rejected = 7;
inline constexpr int exit_material_mapping_missing = 9;

int run_ecad(const ImportOptions& options);
int run_step(const ImportOptions& options);
// Format-specific help extension points; each returned string supplies its own newlines.
std::string ecad_help();
std::string step_help();
void write_defects(const std::filesystem::path& file,
                   const std::string& code,
                   const std::string& message);
void derive_output_paths(ImportOptions& options);
std::filesystem::path import_report_directory(const std::filesystem::path& output);
} // namespace chipcae::importer
