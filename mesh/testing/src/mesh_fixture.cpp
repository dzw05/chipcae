#include "chipcae/mesh/testing/mesh_fixture.hpp"
namespace chipcae::mesh::testing {
Expected<void> save_mesh_fixture(const HexMeshArrays&, const std::filesystem::path&) {
    return Error{"NOT_IMPLEMENTED", "mesh fixture save not implemented (test-first stub)"};
}
Expected<HexMeshArrays> load_mesh_fixture(const std::filesystem::path&) {
    return Error{"NOT_IMPLEMENTED", "mesh fixture load not implemented (test-first stub)"};
}
} // namespace chipcae::mesh::testing
