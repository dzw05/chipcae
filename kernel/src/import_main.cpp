#include "import_common.hpp"

#include <iostream>

namespace {
void help() {
    // Empty format-specific help keeps the baseline output byte-identical.
    std::cout << "usage: chipcae-import <in.step|in.xml> -o <out.stack.json> "
                 "[--polyset <out.polyset>] [--report <defects.json>] [--material-map m.json] "
                 "[--accept-b-tier]\n"
              << chipcae::importer::ecad_help() << chipcae::importer::step_help();
}
} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h")) {
        help();
        return chipcae::importer::CliExit::success;
    }
    chipcae::importer::ImportOptions options;
    auto& input = options.input;
    auto& output = options.output;
    auto& polyset = options.polyset;
    auto& report = options.report;
    auto& material_map = options.material_map;
    auto& accept_b = options.accept_b_tier;
    std::string reserved_argument;
    std::string unimplemented_argument;
    bool has_accept = false;
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
                return chipcae::importer::CliExit::usage;
            }
        } else if (arg == "--polyset") {
            if (!next(polyset)) {
                help();
                return chipcae::importer::CliExit::usage;
            }
        } else if (arg == "--report") {
            if (!next(report)) {
                help();
                return chipcae::importer::CliExit::usage;
            }
        } else if (arg == "--material-map") {
            if (!next(material_map)) {
                help();
                return chipcae::importer::CliExit::usage;
            }
        } else if (arg == "--accept-b-tier") {
            accept_b = true;
        } else if (arg == "--accept" || arg == "--t-min" || arg == "--stack-override" ||
                   arg == "--assembly") {
            std::filesystem::path value;
            if (!next(value)) {
                std::cerr << "unknown argument: " << arg << '\n';
                return chipcae::importer::CliExit::usage;
            }
            // --t-min consumes a placeholder value; numeric parsing belongs to I-STEP-B.
            if (reserved_argument.empty())
                reserved_argument = arg;
            if ((arg == "--t-min" || arg == "--assembly") && unimplemented_argument.empty())
                unimplemented_argument = arg;
            if (arg == "--accept")
                has_accept = true;
            if (arg == "--accept") {
                options.accept_file = value;
            } else if (arg == "--stack-override") {
                options.stack_override = value;
            } else if (arg == "--assembly") {
                options.assembly = value;
            }
        } else if (!arg.empty() && arg[0] != '-' && input.empty()) {
            input = arg;
        } else {
            std::cerr << "unknown argument: " << arg << '\n';
            return chipcae::importer::CliExit::usage;
        }
    }
    const bool normal_parse_path = !input.empty() && !output.empty();
    const bool assembly_parse_path = !options.assembly.empty() && !output.empty();
    // Without an input, only the planned assembly invocation has a reserved diagnostic.
    if (!normal_parse_path && assembly_parse_path)
        reserved_argument = "--assembly";
    const bool reserved_parse_path = normal_parse_path || assembly_parse_path;
    if (!reserved_argument.empty() && !reserved_parse_path) {
        std::cerr << "unknown argument: " << reserved_argument << '\n';
        return chipcae::importer::CliExit::usage;
    }
    if (accept_b && has_accept) {
        // The conflict contract is a silent usage failure in either flag order.
        return chipcae::importer::CliExit::usage;
    }
    const auto ext = input.extension().string();
    const bool step_input = ext == ".step" || ext == ".stp";
    // ECAD implements accept/override; preserve reserved diagnostics for STEP.
    if (step_input && !reserved_argument.empty())
        unimplemented_argument = reserved_argument;
    if (!normal_parse_path && assembly_parse_path)
        unimplemented_argument = "--assembly";
    if (!unimplemented_argument.empty()) {
        std::cerr << "not implemented yet: " << unimplemented_argument << '\n';
        return chipcae::importer::CliExit::usage;
    }
    if (input.empty() || output.empty()) {
        help();
        return chipcae::importer::CliExit::usage;
    }
    chipcae::importer::derive_output_paths(options);
    if (step_input)
        return chipcae::importer::run_step(options);
    return chipcae::importer::run_ecad(options);
}
