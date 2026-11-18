#include "assembly_seam_support.hpp"

#include <iostream>
#include <sstream>

int main() {
    using chipcae::importer::assembly_error_exit;
    const auto prefixed = chipcae::importer::assembly_failure_message(
        "INPUT_INVALID", "CUT_UNSUPPORTED: outline needs one ring");
    if (prefixed != "CUT_UNSUPPORTED: outline needs one ring" ||
        assembly_error_exit(prefixed) != 13 ||
        assembly_error_exit(chipcae::importer::assembly_failure_message("INPUT_INVALID", "x")) !=
            14)
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
        if (assembly_error_exit("INPUT_INVALID: x", input) != 14 ||
            assembly_error_exit("CUT_UNSUPPORTED: x", cut) != 13 ||
            assembly_error_exit("BOGUS: x", unknown) != 1)
            return 1;
    }
}
