#include "chipcae/geometry/import_report.hpp"
#include "import_cli_contract.hpp"
#include "import_common.hpp"
#ifdef CHIPCAE_ENABLE_ECAD
#include "chipcae/geometry/ecad/ipc2581.hpp"
#endif

#include <iostream>

namespace chipcae::importer {
int run_ecad(const ImportOptions& options) {
    const auto& report = options.report;
#ifdef CHIPCAE_ENABLE_ECAD
    const auto& input = options.input;
    const auto& output = options.output;
    const auto& polyset = options.polyset;
    const auto& material_map = options.material_map;
    const bool accept_b = options.accept_b_tier;
#endif
#ifndef CHIPCAE_ENABLE_ECAD
    write_defects(report, "IMPORT_REJECTED", "ECAD support is disabled");
    std::cerr
        << "IMPORT_REJECTED: ECAD support is disabled; configure with CHIPCAE_ENABLE_ECAD=ON\n";
    return exit_import_rejected;
#else
    if (material_map.empty()) {
        write_defects(report, "MATERIAL_MAPPING_MISSING", "--material-map is required");
        std::cerr << "MATERIAL_MAPPING_MISSING: --material-map is required\n";
        return exit_material_mapping_missing;
    }
    auto imported = chipcae::geometry::ecad::read_ipc2581(input);
    if (!imported) {
        write_defects(report, "IMPORT_REJECTED", imported.error);
        std::cerr << "IMPORT_REJECTED: " << imported.error << '\n';
        return exit_import_rejected;
    }
    std::string error;
    if (!chipcae::geometry::ecad::apply_material_map(imported.model, material_map, &error)) {
        const bool missing = error.find("MATERIAL_MAPPING_MISSING") != std::string::npos;
        write_defects(report, missing ? "MATERIAL_MAPPING_MISSING" : "IMPORT_REJECTED", error);
        std::cerr << error << '\n';
        return missing ? exit_material_mapping_missing : exit_import_rejected;
    }
    std::filesystem::create_directories(output.parent_path().empty() ? "." : output.parent_path());
    if (!chipcae::geometry::ecad::to_stack(imported.model, output, polyset, &error)) {
        write_defects(report, "IMPORT_REJECTED", error);
        std::cerr << "IMPORT_REJECTED: " << error << '\n';
        return exit_import_rejected;
    }
    const auto report_dir = import_report_directory(output);
    chipcae::geometry::ImportReportHeader header;
    header.b_tier_accepted_all = accept_b;
    chipcae::geometry::write_import_report(report_dir, header, {}, &error);
    write_defects(report, "OK", "");
    return exit_success;
#endif
}

std::string ecad_help() {
    return {};
}
} // namespace chipcae::importer
