#include "import_common.hpp"

#include <fstream>

namespace chipcae::importer {
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
