#include "import_common.hpp"

#include "chipcae/geometry/import_report.hpp"

#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <yaml-cpp/yaml.h>

namespace chipcae::importer {
namespace {
std::string defect_json_quote(const std::string& s) {
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
} // namespace

void write_defects(const std::filesystem::path& file,
                   const std::string& code,
                   const std::string& message) {
    write_defects(file, code, message, {});
}
void write_defect(std::ostream& out, const geometry::Defect& d) {
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
    out << ",\"details\":" << defect_json_quote(d.details);
    out << '}';
}
void write_defects(const std::filesystem::path& file,
                   const std::string& code,
                   const std::string& message,
                   const std::vector<geometry::Defect>& defects) {
    if (file.empty())
        return;
    std::ofstream out(file);
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
    out.close();
    if (!out)
        throw std::runtime_error("cannot write defects output");
}
std::optional<int> protocol_error_code(const std::string& name, std::istream& input) {
    try {
        const auto document = YAML::Load(input);
        for (const auto& entry : document["errors"])
            if (entry["name"].as<std::string>() == name)
                return entry["code"].as<int>();
    } catch (const YAML::Exception&) {
    }
    return std::nullopt;
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
