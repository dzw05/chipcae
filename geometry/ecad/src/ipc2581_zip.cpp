#include "chipcae/geometry/ecad/ipc2581_zip.hpp"
namespace chipcae::geometry::ecad {
// Test-first placeholder: deliberately provides no ZIP import behavior.
ImportResult read_ipc2581_zip(const std::filesystem::path&) {
    ImportResult result;
    result.error = "IMPORT_REJECTED: IPC ZIP reader not implemented (test-first stub)";
    return result;
}
} // namespace chipcae::geometry::ecad
