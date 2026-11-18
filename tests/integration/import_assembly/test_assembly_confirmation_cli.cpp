// Exercise the real CLI orchestration with a deterministic STEP reader seam.
// The unmapped body produces a B-tier defect without requiring OCCT.
#define import_step assembly_test_import_step
#define step_import_available assembly_test_step_available
#include "assembly_seam_support.hpp"
#undef import_step
#undef step_import_available

#if defined(CHIPCAE_ENABLE_JSONCONS) && defined(CHIPCAE_ENABLE_ECAD)
namespace chipcae::geometry {
bool assembly_test_step_available() noexcept {
    return true;
}
StepImportResult assembly_test_import_step(const std::filesystem::path&, const StepImportOptions&) {
    StepImportResult result;
    result.body_names = {"unmapped-body"};
    return result;
}
} // namespace chipcae::geometry

#include "../../../kernel/src/stack_writer.cpp"
#define main assembly_cli_main
#include "../../../kernel/src/import_main.cpp"
#undef main
namespace chipcae::importer {
int run_ecad(const ImportOptions&) {
    return 99;
}
int run_step(const ImportOptions&) {
    return 99;
}
std::string ecad_help() {
    return {};
}
std::string step_help() {
    return {};
}
} // namespace chipcae::importer
#endif

int main(int argc, char** argv) {
#if defined(CHIPCAE_ENABLE_JSONCONS) && defined(CHIPCAE_ENABLE_ECAD)
    if (argc > 1)
        return assembly_cli_main(argc, argv);
    const auto script = std::filesystem::path(CHIPCAE_SOURCE_DIR) /
                        "tests/integration/import_assembly/confirmation_cli.py";
    const std::string command = "python3 '" + script.string() + "' '" + argv[0] + "'";
    return std::system(command.c_str()) == 0 ? 0 : 1;
#else
    (void)argc;
    (void)argv;
    return 77;
#endif
}
