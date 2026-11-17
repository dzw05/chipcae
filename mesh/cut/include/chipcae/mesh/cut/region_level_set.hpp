#pragma once
#include "chipcae/mesh/background/hex_mesh_arrays.hpp"
#include "chipcae/mesh/cut/cut_layer.hpp"
#include "chipcae/schema/stack.hpp"

#include <vector>

namespace chipcae::mesh {
struct RegionLSOptions {
    Real inside_tol_rel = 1e-12;
};
// Define effective_body(r) as r.body when nonempty, otherwise r.name (exact,
// case-sensitive strings). For each layer index li and effective body b, the
// source group is every region index ri in stack.layers[li].regions for which
// footprint.polygon is nonempty and effective_body(region) == b. Distinct region
// names with the same effective body belong to the same group, including a name
// fallback that equals another region's explicit body. Box-only regions do not.
// Emit one entry per nonempty source group; phi represents its polygon union.
// Order groups by layer index, then by the smallest source region index in each
// group. The group body maps to HexMeshArrays::attr_table.bodies.at(b).
// Returned entries contain body IDs and level-set values only; they carry no
// layer or source-region metadata. Body identity alone does not identify a
// source group because the same body may recur across layers.
std::vector<cut::BodyLevelSet>
region_level_sets(const StackModel&, const HexMeshArrays&, const RegionLSOptions&);
} // namespace chipcae::mesh
