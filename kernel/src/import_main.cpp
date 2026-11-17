#include "chipcae/geometry/import_report.hpp"
#ifdef CHIPCAE_ENABLE_ECAD
#include "chipcae/geometry/ecad/ipc2581.hpp"
#endif

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {
void write_defects(const std::filesystem::path& file,
                   const std::string& code,
                   const std::string& message) {
    std::ofstream out(file);
    if (out)
        out << "{\"code\":\"" << code << "\",\"message\":\"" << message << "\",\"defects\":[]}\n";
}
void help() {
    std::cout << "usage: chipcae-import <in.step|in.xml> -o <out.stack.json> "
                 "[--polyset <out.polyset>] [--report <defects.json>] [--material-map m.json] "
                 "[--accept-b-tier]\n";
}
} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h")) {
        help();
        return 0;
    }
    std::filesystem::path input, output, polyset, report, material_map;
    bool accept_b = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        auto next = [&](std::filesystem::path& path) {
            if (i + 1 >= argc)
                return false;
            path = argv[++i];
            return true;
        };
        if (arg == "-o" || arg == "--out") {
            if (!next(output)) {
                help();
                return 2;
            }
        } else if (arg == "--polyset") {
            if (!next(polyset)) {
                help();
                return 2;
            }
        } else if (arg == "--report") {
            if (!next(report)) {
                help();
                return 2;
            }
        } else if (arg == "--material-map") {
            if (!next(material_map)) {
                help();
                return 2;
            }
        } else if (arg == "--accept-b-tier") {
            accept_b = true;
        } else if (!arg.empty() && arg[0] != '-' && input.empty()) {
            input = arg;
        } else {
            std::cerr << "unknown argument: " << arg << '\n';
            return 2;
        }
    }
    if (input.empty() || output.empty()) {
        help();
        return 2;
    }
    if (polyset.empty()) {
        polyset = output;
        polyset.replace_extension(".polyset");
    }
    if (report.empty()) {
        report = output.parent_path() / "defects.json";
        if (report.empty())
            report = "defects.json";
    }
    const auto ext = input.extension().string();
    if (ext == ".step" || ext == ".stp") {
        write_defects(report, "IMPORT_REJECTED", "STEP import is unavailable in this build");
        std::cerr << "IMPORT_REJECTED: STEP importer is unavailable\n";
        return 7;
    }
#ifndef CHIPCAE_ENABLE_ECAD
    write_defects(report, "IMPORT_REJECTED", "ECAD support is disabled");
    std::cerr
        << "IMPORT_REJECTED: ECAD support is disabled; configure with CHIPCAE_ENABLE_ECAD=ON\n";
    return 7;
#else
    if (material_map.empty()) {
        write_defects(report, "MATERIAL_MAPPING_MISSING", "--material-map is required");
        std::cerr << "MATERIAL_MAPPING_MISSING: --material-map is required\n";
        return 9;
    }
    auto imported = chipcae::geometry::ecad::read_ipc2581(input);
    if (!imported) {
        write_defects(report, "IMPORT_REJECTED", imported.error);
        std::cerr << "IMPORT_REJECTED: " << imported.error << '\n';
        return 7;
    }
    std::string error;
    if (!chipcae::geometry::ecad::apply_material_map(imported.model, material_map, &error)) {
        const bool missing = error.find("MATERIAL_MAPPING_MISSING") != std::string::npos;
        write_defects(report, missing ? "MATERIAL_MAPPING_MISSING" : "IMPORT_REJECTED", error);
        std::cerr << error << '\n';
        return missing ? 9 : 7;
    }
    std::filesystem::create_directories(output.parent_path().empty() ? "." : output.parent_path());
    if (!chipcae::geometry::ecad::to_stack(imported.model, output, polyset, &error)) {
        write_defects(report, "IMPORT_REJECTED", error);
        std::cerr << "IMPORT_REJECTED: " << error << '\n';
        return 7;
    }
    const auto report_dir =
        output.parent_path().empty() ? std::filesystem::path(".") : output.parent_path();
    chipcae::geometry::ImportReportHeader header;
    header.b_tier_accepted_all = accept_b;
    chipcae::geometry::write_import_report(report_dir, header, {}, &error);
    write_defects(report, "OK", "");
    return 0;
#endif
}
