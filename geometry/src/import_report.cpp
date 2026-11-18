#include "chipcae/geometry/import_report.hpp"

#include <fstream>
#include <iomanip>

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
} // namespace
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
        j << "{\"defect_id\":" << quote(id) << ",\"tier\":" << quote(tier)
          << ",\"kind\":" << quote(d.kind) << ",\"reason\":" << static_cast<unsigned>(d.reason)
          << ",\"dimension\":" << quote(d.dimension) << ",\"size\":" << std::setprecision(17)
          << d.size << ",\"suggestion\":" << quote(d.suggestion)
          << ",\"action\":" << quote(d.action)
          << ",\"rejection_reason\":" << quote(d.rejection_reason)
          << ",\"material_ambiguous\":" << (d.material_ambiguous ? "true" : "false") << '}';
        c << quote(id) << ',' << quote(tier) << ',' << quote(d.kind) << ','
          << static_cast<unsigned>(d.reason) << ',' << quote(d.dimension) << ','
          << std::setprecision(17) << d.size << ',' << quote(d.suggestion) << ',' << quote(d.action)
          << ',' << quote(d.rejection_reason) << ',' << (d.material_ambiguous ? "true" : "false")
          << '\n';
    }
    j << "]}\n";
    return static_cast<bool>(j) && static_cast<bool>(c);
}
} // namespace chipcae::geometry
