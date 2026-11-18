#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

// Private to chipcae-import; this header is not an installed public API.
namespace chipcae::importer {
struct CliExit {
    static constexpr int success = 0;
    static constexpr int usage = 2;
    static constexpr int import_confirmation_required = 6;
    static constexpr int import_rejected = 7;
    static constexpr int material_mapping_missing = 9;
};

struct ImportOptions {
    std::filesystem::path input, output, polyset, report, material_map;
    bool accept_b_tier = false;
    std::filesystem::path accept_file;
    std::optional<double> t_min;
    std::filesystem::path stack_override, assembly;
    std::vector<std::string> deferred_options;
};

int run_import(ImportOptions options, void (*help)());
int run_ecad(const ImportOptions& options);
int run_step(const ImportOptions& options);
int run_assembly(const ImportOptions& options);
// Format-specific help extension points; each returned string supplies its own newlines.
std::string ecad_help();
std::string step_help();
void write_defects(const std::filesystem::path& file,
                   const std::string& code,
                   const std::string& message);
void derive_output_paths(ImportOptions& options);
std::filesystem::path import_report_directory(const std::filesystem::path& output);
} // namespace chipcae::importer
