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
// Emit one entry per (layer, effective body) with polygon regions, in stack
// layer order, then first occurrence of the body in Layer::regions. An empty
// region body uses its region name, as in the background mesh attribute table.
// Each entry unions all polygon regions in that layer with that effective body.
// Its body equals HexMeshArrays::attr_table.bodies.at(effective_body). Recover
// source regions by matching that body in the corresponding StackModel layer.
// Bodies may recur across layers: traversal order and body together identify
// the source group. Groups without polygon regions produce no entry.
std::vector<BodyLevelSet>
region_level_sets(const StackModel&, const HexMeshArrays&, const RegionLSOptions&);
} // namespace chipcae::mesh
