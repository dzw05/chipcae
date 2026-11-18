#pragma once
#include "chipcae/mesh/background/hex_mesh_arrays.hpp"

#include <filesystem>
namespace chipcae::mesh::testing {
// Save deterministically, overwriting path. Invalid meshes return INPUT_INVALID.
chipcae::Expected<void> save_mesh_fixture(const HexMeshArrays&, const std::filesystem::path&);
// Load a complete validated mesh; malformed input returns INPUT_INVALID.
chipcae::Expected<HexMeshArrays> load_mesh_fixture(const std::filesystem::path&);
} // namespace chipcae::mesh::testing
