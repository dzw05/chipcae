#include "assembly_seam_support.hpp"

#include <iostream>

int main() {
    using chipcae::importer::assembly_error_exit;
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
}
