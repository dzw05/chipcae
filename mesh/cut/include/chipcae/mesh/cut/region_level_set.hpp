#pragma once
#include "chipcae/mesh/background/hex_mesh_arrays.hpp"
#include "chipcae/schema/stack.hpp"

#include <cstddef>
#include <vector>

namespace chipcae::mesh::cut {
struct BodyLevelSet {
    BodyId body{}; // ID from HexMeshArrays::attr_table.bodies.
    // One value per background mesh vertex, in HexMeshArrays::vertices order
    // (phi[i] corresponds to vertices[3*i .. 3*i+2]); negative inside the union.
    std::vector<Real> phi;
    std::size_t layer_index = 0; // Index into the input StackModel::layers.
};
} // namespace chipcae::mesh::cut

namespace chipcae::mesh {
using cut::BodyLevelSet;

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
// Each entry identifies its source group by (layer_index, body), including when
// the same body recurs across layers. phi samples the source group polygon union.
std::vector<BodyLevelSet>
region_level_sets(const StackModel&, const HexMeshArrays&, const RegionLSOptions&);
} // namespace chipcae::mesh
