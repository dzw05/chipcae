#pragma once
#include "chipcae/mesh/background/hex_mesh_arrays.hpp"

// TEST-FIRST scaffolding: the existing Expected template cannot hold void.
// Keep this error-only specialization local until the public result contract is settled.
namespace chipcae::mesh {
template <>
class Expected<void> {
    Error error_;

public:
    Expected(Error error) : error_(std::move(error)) {}
    explicit operator bool() const { return false; }
    const Error& error() const { return error_; }
};
namespace testing {
Expected<void> save_mesh_fixture(const HexMeshArrays&, const std::filesystem::path&);
Expected<HexMeshArrays> load_mesh_fixture(const std::filesystem::path&);
} // namespace testing
} // namespace chipcae::mesh
