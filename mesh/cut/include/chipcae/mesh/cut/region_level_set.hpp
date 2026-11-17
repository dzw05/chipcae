#pragma once
#include "chipcae/mesh/background/hex_mesh_arrays.hpp"
#include "chipcae/mesh/cut/cut_layer.hpp"
namespace chipcae::mesh {
// TEST-FIRST compile stubs; no level-set behavior implemented.
struct RegionLSOptions {};
using BodyLevelSet = cut::BodyLevelSet;
inline std::vector<BodyLevelSet>
region_level_sets(const StackModel&, const HexMeshArrays&, const RegionLSOptions&) {
    return {};
}
} // namespace chipcae::mesh
