#include "import_common.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>

namespace chipcae::importer {
namespace {
int run_legacy(const ImportOptions& options, void (*help)()) {
    const bool complete = !options.input.empty() && !options.output.empty();
    if (!options.deferred_options.empty() && !complete) {
        std::cerr << "unknown argument: " << options.deferred_options.front() << '\n';
        return CliExit::usage;
    }
    if (options.accept_b_tier &&
        std::find(options.deferred_options.begin(), options.deferred_options.end(), "--accept") !=
            options.deferred_options.end())
        return CliExit::usage;
    if (!options.deferred_options.empty()) {
        std::cerr << "not implemented yet: " << options.deferred_options.front() << '\n';
        return CliExit::usage;
    }
    if (!complete) {
        help();
        return CliExit::usage;
    }
    const auto ext = options.input.extension().string();
    return ext == ".step" || ext == ".stp" ? run_step(options) : run_ecad(options);
}
} // namespace
int run_import(ImportOptions options, void (*help)()) {
    derive_output_paths(options);
    const bool assembly =
        std::find(options.deferred_options.begin(), options.deferred_options.end(), "--assembly") !=
        options.deferred_options.end();
    return assembly ? run_assembly(options) : run_legacy(options, help);
}

void write_defects(const std::filesystem::path& file,
                   const std::string& code,
                   const std::string& message) {
    std::ofstream out(file);
    if (out)
        out << "{\"code\":\"" << code << "\",\"message\":\"" << message << "\",\"defects\":[]}\n";
}
void derive_output_paths(ImportOptions& options) {
    auto& output = options.output;
    auto& polyset = options.polyset;
    auto& report = options.report;
    if (polyset.empty()) {
        polyset = output;
        polyset.replace_extension(".polyset");
    }
    if (report.empty()) {
        report = output.parent_path() / "defects.json";
        if (report.empty())
            report = "defects.json";
    }
}

std::filesystem::path import_report_directory(const std::filesystem::path& output) {
    const auto report_dir =
        output.parent_path().empty() ? std::filesystem::path(".") : output.parent_path();
    return report_dir;
}
} // namespace chipcae::importer
