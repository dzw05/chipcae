#pragma once

#include "chipcae/geometry/import_report.hpp"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <vector>
#if defined(CHIPCAE_ENABLE_JSONCONS)
#include <jsoncons/json.hpp>
#endif

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
};

int run_ecad(const ImportOptions& options);
int run_step(const ImportOptions& options);
int run_assembly(const ImportOptions& options);

inline std::string defect_json_quote(const std::string& s) {
    std::ostringstream out;
    out << '"';
    for (unsigned char c : s) {
        if (c == '"' || c == '\\')
            out << '\\' << c;
        else if (c < 0x20)
            out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(c);
        else
            out << c;
    }
    out << '"';
    return out.str();
}

inline void write_defect(std::ostream& out, const geometry::Defect& d) {
    const auto id = geometry::defect_id(d), tier = geometry::tier_name(d.tier);
    out << "{\"defect_id\":" << defect_json_quote(id) << ",\"tier\":" << defect_json_quote(tier)
        << ",\"kind\":" << defect_json_quote(d.kind)
        << ",\"reason\":" << static_cast<unsigned>(d.reason)
        << ",\"dimension\":" << defect_json_quote(d.dimension)
        << ",\"size\":" << std::setprecision(17) << d.size
        << ",\"suggestion\":" << defect_json_quote(d.suggestion)
        << ",\"action\":" << defect_json_quote(d.action)
        << ",\"rejection_reason\":" << defect_json_quote(d.rejection_reason)
        << ",\"material_ambiguous\":" << (d.material_ambiguous ? "true" : "false");
    out << '}';
}
inline void write_defects_json(std::ostream& out,
                               const std::string& code,
                               const std::string& message,
                               const std::vector<geometry::Defect>& defects) {
    out << "{\"code\":" << defect_json_quote(code) << ",\"message\":" << defect_json_quote(message)
        << ",\"defects\":[";
    bool first = true;
    for (const auto& d : defects) {
        if (!first)
            out << ',';
        first = false;
        write_defect(out, d);
    }
    out << "]}\n";
}

inline void write_defects(const std::filesystem::path& file,
                          const std::string& code,
                          const std::string& message,
                          const std::vector<geometry::Defect>& defects) {
    if (file.empty())
        return;
    std::ofstream out(file);
    write_defects_json(out, code, message, defects);
    out.close();
    if (!out)
        throw std::runtime_error("cannot write defects output");
}

#if defined(CHIPCAE_ENABLE_JSONCONS)
inline std::vector<std::string> read_acceptance_list(const ImportOptions& options,
                                                     const std::filesystem::path& directory) {
    auto file = options.accept_file;
    if (file.empty() && !options.accept_b_tier) {
        auto candidate = directory / "import.accept.json";
        if (std::filesystem::exists(candidate))
            file = candidate;
    }
    std::vector<std::string> accepted;
    if (!file.empty()) {
        std::ifstream input(file);
        auto document = jsoncons::json::parse(input);
        for (const auto& item : document["accept"].array_range())
            accepted.push_back(item["defect_id"].as<std::string>());
    }
    return accepted;
}

#endif
// Format-specific help extension points; each returned string supplies its own newlines.
std::string ecad_help();
std::string step_help();
void write_defects(const std::filesystem::path& file,
                   const std::string& code,
                   const std::string& message);
void derive_output_paths(ImportOptions& options);
std::filesystem::path import_report_directory(const std::filesystem::path& output);
} // namespace chipcae::importer
