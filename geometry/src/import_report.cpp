#include "chipcae/geometry/import_report.hpp"

#include <fstream>
#include <iomanip>
#include <sstream>

namespace chipcae::geometry {
namespace {
std::string quote(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"')
            out += '"';
        out += c;
    }
    return out + '"';
}
std::string json_quote(const std::string& s) {
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
void strings(std::ostream& out, const std::vector<std::string>& values) {
    out << '[';
    bool first = true;
    for (const auto& value : values) {
        if (!first)
            out << ',';
        first = false;
        out << json_quote(value);
    }
    out << ']';
}
} // namespace
bool write_import_report(const std::filesystem::path& dir,
                         const ImportReportHeader& h,
                         const std::vector<Defect>& ds,
                         std::string* error,
                         const ImportReportParts* parts) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        if (error)
            *error = ec.message();
        return false;
    }
    unsigned counts[4]{};
    for (const auto& d : ds)
        ++counts[static_cast<unsigned>(d.tier)];
    std::ofstream j(dir / "import_report.json"), c(dir / "import_report.csv");
    if (!j || !c) {
        if (error)
            *error = "cannot open report output";
        return false;
    }
    j << std::setprecision(17) << "{\"L\":" << h.L << ",\"t_min\":" << h.t_min
      << ",\"eps_weld\":" << h.tolerances.eps_weld << ",\"tau_A\":" << h.tolerances.tau_A
      << ",\"tau_B\":" << h.tolerances.tau_B << ",\"delta_f\":" << h.tolerances.delta_f
      << ",\"tier_counts\":{\"Weld\":" << counts[0] << ",\"A\":" << counts[1]
      << ",\"B\":" << counts[2] << ",\"C\":" << counts[3]
      << "},\"b_tier_accepted\":" << (h.b_tier_accepted_all ? "\"all\"" : "null")
      << ",\"defects\":[";
    c << "defect_id,tier,kind,reason,dimension,size,suggestion,action,rejection_reason,material_"
         "ambiguous\n";
    bool first = true;
    for (const auto& d : ds) {
        const auto id = defect_id(d), tier = tier_name(d.tier);
        if (!first)
            j << ',';
        first = false;
        j << "{\"defect_id\":" << json_quote(id) << ",\"tier\":" << json_quote(tier)
          << ",\"kind\":" << json_quote(d.kind) << ",\"reason\":" << static_cast<unsigned>(d.reason)
          << ",\"dimension\":" << json_quote(d.dimension) << ",\"size\":" << std::setprecision(17)
          << d.size << ",\"suggestion\":" << json_quote(d.suggestion)
          << ",\"action\":" << json_quote(d.action)
          << ",\"rejection_reason\":" << json_quote(d.rejection_reason)
          << ",\"material_ambiguous\":" << (d.material_ambiguous ? "true" : "false");
        if (parts || !d.details.empty())
            j << ",\"details\":" << json_quote(d.details);
        j << '}';
        c << quote(id) << ',' << quote(tier) << ',' << quote(d.kind) << ','
          << static_cast<unsigned>(d.reason) << ',' << quote(d.dimension) << ','
          << std::setprecision(17) << d.size << ',' << quote(d.suggestion) << ',' << quote(d.action)
          << ',' << quote(d.rejection_reason) << ',' << (d.material_ambiguous ? "true" : "false")
          << '\n';
    }
    j << ']';
    if (parts) {
        j << ",\"parts\":{\"split_layers\":{";
        bool first = true;
        for (const auto& [name, targets] : parts->split_layers) {
            if (!first)
                j << ',';
            first = false;
            j << json_quote(name) << ':';
            strings(j, targets);
        }
        j << "},\"rewrite_targets\":[";
        first = true;
        for (const auto& [from, targets] : parts->rewrite_targets) {
            if (!first)
                j << ',';
            first = false;
            j << "{\"from\":" << json_quote(from) << ",\"to\":";
            strings(j, targets);
            j << '}';
        }
        j << "],\"material_approx\":[";
        first = true;
        for (const auto& id : parts->material_approx) {
            if (!first)
                j << ',';
            first = false;
            j << "{\"id\":" << json_quote(id)
              << ",\"material_approx\":true,\"label\":\"近似材料\"}";
        }
        j << "]}";
    }
    j << "}\n";
    return static_cast<bool>(j) && static_cast<bool>(c);
}
} // namespace chipcae::geometry
