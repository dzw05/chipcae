#include "assembly_seam_support.hpp"

#include <iostream>
#include <sstream>

int main() {
#if defined(CHIPCAE_ENABLE_JSONCONS)
    // The legacy ECAD/STEP report writer ignores unavailable output paths.
    try {
        chipcae::importer::write_defects(std::filesystem::path(CHIPCAE_SOURCE_DIR), "X", "x");
    } catch (...) {
        std::cerr << "legacy report writer must not throw on output failure\n";
        return 1;
    }
    using chipcae::importer::assembly_error_exit;
    using chipcae::importer::protocol_error_exit;
    // Real SP3 merge errors retain their message and protocol exit code.
    const auto merged = chipcae::geometry::merge_parts({}, {}, {-1, 1e-6, 1e-6});
    if (merged || merged.error().message != "INPUT_INVALID: invalid MergeOptions" ||
        assembly_error_exit(merged.error().message) != 14)
        return 1;
    const std::string outline_error = "outline.unsupported: non-convex outline";
    if (assembly_error_exit(outline_error) != 13 ||
        chipcae::importer::import_failure_message("INTERNAL", outline_error) != outline_error) {
        std::cerr << "unsupported merge outlines must preserve the message and map to exit 13\n";
        return 1;
    }
    for (const auto& message :
         {"INPUT_INVALID: bad STEP", "CUT_UNSUPPORTED: STEP", "BOGUS: STEP"}) {
        const auto preserved =
            chipcae::importer::import_failure_message("IMPORT_REJECTED", message);
        if (preserved != message || assembly_error_exit(preserved) != assembly_error_exit(message))
            return 1;
    }
    if (chipcae::importer::import_failure_message("IMPORT_REJECTED", "invalid STEP") !=
        "IMPORT_REJECTED: invalid STEP")
        return 1;
    const auto prefixed = chipcae::importer::import_failure_message(
        "IMPORT_REJECTED", "CUT_UNSUPPORTED: outline needs one ring");
    if (prefixed != "CUT_UNSUPPORTED: outline needs one ring" ||
        assembly_error_exit(prefixed) != 13 ||
        assembly_error_exit(chipcae::importer::import_failure_message("INPUT_INVALID", "x")) != 14)
        return 1;
    if (assembly_error_exit("IMPORT_CONFIRMATION_REQUIRED: synthetic B-tier defect") != 6)
        return 1;
    if (assembly_error_exit("BOGUS: x") != 1) {
        std::cerr << "unknown assembly error prefix must map to INTERNAL / exit 1\n";
        return 1;
    }
    if (assembly_error_exit("INPUT_INVALID: part id collides with Core") != 14 ||
        assembly_error_exit("CUT_UNSUPPORTED: outline needs one ring") != 13 ||
        assembly_error_exit("IMPORT_REJECTED: invalid attachment") != 7) {
        std::cerr << "assembly threshold errors must use protocol/errors.yaml exit codes\n";
        return 1;
    }
    for (const auto& yaml :
         {"errors:\n  - {code: \"14\", # quoted code\n     name: INPUT_INVALID}\n"
          "  - name: 'CUT_UNSUPPORTED' # comment\n    code: '13'\n",
          "errors:\n  - code: 14\n    name: INPUT_INVALID\n  - code: 13\n    "
          "name: CUT_UNSUPPORTED\n",
          "errors:\n  - name: INPUT_INVALID\n    code: 14\n  - name: "
          "CUT_UNSUPPORTED\n    code: 13\n",
          "errors:\n  - {code: 14, name: INPUT_INVALID}\n  - {name: "
          "CUT_UNSUPPORTED, code: 13}\n"}) {
        std::istringstream input(yaml), cut(yaml), unknown(yaml);
        if (protocol_error_exit("INPUT_INVALID: x", input) != 14 ||
            protocol_error_exit("CUT_UNSUPPORTED: x", cut) != 13 ||
            protocol_error_exit("BOGUS: x", unknown) != 1)
            return 1;
    }
#else
    return 77;
#endif
}
