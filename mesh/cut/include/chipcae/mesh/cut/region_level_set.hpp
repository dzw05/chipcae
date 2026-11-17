#pragma once
#include "chipcae/mesh/background/hex_mesh_arrays.hpp"
#include "chipcae/mesh/cut/cut_layer.hpp"
#include "chipcae/schema/stack.hpp"

#include <vector>

namespace chipcae::mesh {
struct RegionLSOptions {
    Real inside_tol_rel = 1e-12;
};
// Reuse the cut pipeline's level-set representation.
using BodyLevelSet = cut::BodyLevelSet;
std::vector<BodyLevelSet>
region_level_sets(const StackModel&, const HexMeshArrays&, const RegionLSOptions&);
} // namespace chipcae::mesh
