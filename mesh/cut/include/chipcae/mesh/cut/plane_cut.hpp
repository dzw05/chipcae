#pragma once
#include "cut_layer.hpp"
namespace chipcae::mesh::cut {
struct Plane {
    BodyId body = 0;
    std::array<Real, 3> n{1, 0, 0};
    Real d = 0;
};
// Repeated body IDs mean intersection of half-spaces: piecewise planar fillets.
struct CutGeometry {
    std::vector<Plane> planes;
};
struct CutOptions {
    Real snap_phi = 1e-14, small_sub_vf = 1e-8;
    int quad_degree = 2;
    // CVs below this sub-volume fraction are recorded in small_subs, without merging;
    // distinct from the design merge threshold (0.1).
    bool keep_void = false;
    bool export_polyhedra = false;
};
// Transactional mutable overload removes void cells and remaps local indices.
Expected<CutLayerArrays> build_cut_layer(HexMeshArrays&, const CutGeometry&, const CutOptions&);
// Const overload leaves mesh untouched; void_elem describes the removal plan.
Expected<CutLayerArrays>
build_cut_layer(const HexMeshArrays&, const CutGeometry&, const CutOptions&);
} // namespace chipcae::mesh::cut
