#include "import_common.hpp"

#include <iostream>

namespace chipcae::importer {
int run_step(const ImportOptions& options) {
    const auto& report = options.report;
    write_defects(report, "IMPORT_REJECTED", "STEP import is unavailable in this build");
    std::cerr << "IMPORT_REJECTED: STEP importer is unavailable\n";
    return 7;
}

std::string step_help() {
    return {};
}
} // namespace chipcae::importer
