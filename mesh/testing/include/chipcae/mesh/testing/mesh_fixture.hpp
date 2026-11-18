#pragma once
#include "chipcae/mesh/background/hex_mesh_arrays.hpp"

#include <filesystem>
#include <optional>
#include <utility>
#include <variant>

namespace chipcae::mesh {
// Status result for fixture writes; the shared primary template stores a value.
template <>
class Expected<void> {
    std::optional<Error> error_;

public:
    Expected() = default;
    Expected(Error error) : error_(std::move(error)) {}
    explicit operator bool() const { return !error_; }
    void value() const {
        if (error_)
            throw std::bad_variant_access{};
    }
    const Error& error() const { return error_.value(); }
};
} // namespace chipcae::mesh

namespace chipcae::mesh::testing {
// Save deterministically, overwriting path. Invalid meshes return INPUT_INVALID.
Expected<void> save_mesh_fixture(const HexMeshArrays&, const std::filesystem::path&);
// Load a complete validated mesh; malformed input returns INPUT_INVALID.
Expected<HexMeshArrays> load_mesh_fixture(const std::filesystem::path&);
} // namespace chipcae::mesh::testing
