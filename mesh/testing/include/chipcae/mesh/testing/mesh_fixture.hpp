#pragma once
#include "chipcae/mesh/background/hex_mesh_arrays.hpp"

// The frozen mesh Expected<T> uses variant<T, Error>; void needs a specialization.
namespace chipcae::mesh {
template <>
class Expected<void> {
    std::optional<Error> error_;

public:
    Expected() = default;
    Expected(Error error) : error_(std::move(error)) {}
    explicit operator bool() const { return !error_; }
    const Error& error() const { return error_.value(); }
};
namespace testing {
// Save deterministically, overwriting path. Invalid meshes return INPUT_INVALID.
Expected<void> save_mesh_fixture(const HexMeshArrays&, const std::filesystem::path&);
// Load a complete validated mesh; malformed input returns INPUT_INVALID.
Expected<HexMeshArrays> load_mesh_fixture(const std::filesystem::path&);
} // namespace testing
} // namespace chipcae::mesh
