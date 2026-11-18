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
#ifndef CHIPCAE_ASSEMBLY_GEOMETRY_AVAILABLE
    // SP0 merge leaves the original board intact; unrelated failures propagate.
    chipcae::geometry::StackModel board;
    board.id = "original-board";
    auto merged = chipcae::importer::assembly_placeholder_result(
        [&] { return chipcae::geometry::merge_parts(board, {}, {}); },
        chipcae::geometry::Expected<chipcae::geometry::MergeResult>{
            chipcae::geometry::MergeResult{board, {}}},
        "not implemented: merge_parts");
    if (!merged || merged.value().stack.id != "original-board")
        return 1;
    try {
        chipcae::importer::assembly_placeholder_result(
            []() -> int { throw std::logic_error("unexpected geometry failure"); },
            0,
            "not implemented: slice_prismatic");
        return 1;
    } catch (const std::logic_error& error) {
        if (std::string(error.what()) != "unexpected geometry failure")
            return 1;
    }
#endif
    using chipcae::importer::assembly_error_exit;
    using chipcae::importer::protocol_error_exit;
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
