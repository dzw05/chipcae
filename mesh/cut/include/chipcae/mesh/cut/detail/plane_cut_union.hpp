#pragma once

#include "chipcae/mesh/cut/plane_cut.hpp"

namespace chipcae::mesh::cut::detail {
struct UnionPiece {
    std::vector<Plane> planes;
    std::vector<bool> internal;
};

Expected<CutLayerArrays> build_cut_layer_union(HexMeshArrays& mesh,
                                               const std::vector<UnionPiece>& pieces,
                                               const CutGeometry& extra,
                                               const CutOptions& options,
                                               const std::vector<Real>& outline_phi);
} // namespace chipcae::mesh::cut::detail
