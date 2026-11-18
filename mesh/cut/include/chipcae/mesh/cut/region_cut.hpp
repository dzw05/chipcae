#pragma once
#include "chipcae/mesh/background/hex_mesh_arrays.hpp"
#include "chipcae/mesh/cut/plane_cut.hpp"
#include "chipcae/schema/stack.hpp"

namespace chipcae::mesh::cut {
// Planes retain n dot x - d <= 0. For axis-aligned edges, snap coordinate c to
// the nearest mesh axis within GridOptions{}.snap_tol, then d = n[axis] * c.
// Thus an outward negative unit normal uses the negated snapped coordinate.
Expected<CutGeometry> region_cut_planes(const StackModel&, const HexMeshArrays&);
} // namespace chipcae::mesh::cut
