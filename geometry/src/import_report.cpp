#include "chipcae/geometry/import_report.hpp"

#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace chipcae::geometry {
namespace {
std::string csv_quote(const std::string& s) {
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
} // namespace
std::string defect_size_unit(const Defect& d) {
    if (d.dimension == "L" || d.dimension == "t_min" || d.dimension == "measured" ||
        d.dimension == "delta")
        return "m";
    return "1";
}

bool write_import_report(const std::filesystem::path& dir,
                         const ImportReportHeader& h,
                         const std::vector<Defect>& ds,
                         std::string* error) {
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
    j << std::setprecision(17) << "{\"schema_version\":\"import-report/1.0\",\"L\":" << h.L
      << ",\"t_min\":" << h.t_min << ",\"eps_weld\":" << h.tolerances.eps_weld
      << ",\"tau_A\":" << h.tolerances.tau_A << ",\"tau_B\":" << h.tolerances.tau_B
      << ",\"delta_f\":" << h.tolerances.delta_f << ",\"tier_counts\":{\"Weld\":" << counts[0]
      << ",\"A\":" << counts[1] << ",\"B\":" << counts[2] << ",\"C\":" << counts[3]
      << "},\"b_tier_accepted\":" << (h.b_tier_accepted_all ? "\"all\"" : "null");
    if (h.welded_interfaces)
        j << ",\"welded_interfaces\":" << *h.welded_interfaces;
    j << ",\"defects\":[";
    c << "defect_id,tier,kind,reason,dimension,size,size_unit,suggestion,action,rejection_reason,"
         "material_ambiguous\n";
    bool first = true;
    for (const auto& d : ds) {
        const auto id = defect_id(d), tier = tier_name(d.tier);
        if (!first)
            j << ',';
        first = false;
        j << "{\"defect_id\":" << json_quote(id) << ",\"tier\":" << json_quote(tier)
          << ",\"kind\":" << json_quote(d.kind) << ",\"reason\":" << static_cast<unsigned>(d.reason)
          << ",\"dimension\":" << json_quote(d.dimension) << ",\"size\":";
        if (std::isfinite(d.size))
            j << std::setprecision(17) << d.size;
        else
            j << "null";
        j << ",\"size_unit\":" << json_quote(defect_size_unit(d))
          << ",\"suggestion\":" << json_quote(d.suggestion)
          << ",\"action\":" << json_quote(d.action)
          << ",\"rejection_reason\":" << json_quote(d.rejection_reason)
          << ",\"material_ambiguous\":" << (d.material_ambiguous ? "true" : "false") << '}';
        c << csv_quote(id) << ',' << csv_quote(tier) << ',' << csv_quote(d.kind) << ','
          << static_cast<unsigned>(d.reason) << ',' << csv_quote(d.dimension) << ','
          << std::setprecision(17) << d.size << ',' << csv_quote(defect_size_unit(d)) << ','
          << csv_quote(d.suggestion) << ',' << csv_quote(d.action) << ','
          << csv_quote(d.rejection_reason) << ',' << (d.material_ambiguous ? "true" : "false")
          << '\n';
    }
    j << "]}\n";
    return static_cast<bool>(j) && static_cast<bool>(c);
}
} // namespace chipcae::geometry
