#pragma once

#include "chipcae/mesh/cut/plane_cut.hpp"

namespace chipcae::mesh::cut::detail {
struct UnionPiece {
    // Convex intersection of half-spaces, bounded in xy; every plane has body == 0.
    std::vector<Plane> planes;
    // Parallel to planes; true marks an internal cap to discard from the union boundary.
    std::vector<bool> internal;
};

// Pieces have mutually disjoint interiors. Extra planes retain build_cut_layer's
// body filtering. outline_phi has one signed xy outline distance per mesh node
// (negative inside). A single piece without internal planes uses the legacy
// path and ignores outline_phi values, after checking its length.
// Success removes/remaps void cells according to options; any error leaves every
// field of mesh unchanged, including errors after export and remapping.
Expected<CutLayerArrays> build_cut_layer_union(HexMeshArrays& mesh,
                                               const std::vector<UnionPiece>& pieces,
                                               const CutGeometry& extra,
                                               const CutOptions& options,
                                               const std::vector<Real>& outline_phi);
} // namespace chipcae::mesh::cut::detail
