#pragma once
#include "chipcae/mesh/background/hex_mesh_arrays.hpp"
#include "chipcae/mesh/cut/plane_cut.hpp"
#include "chipcae/schema/stack.hpp"

namespace chipcae::mesh::cut {
Expected<CutGeometry> region_cut_planes(const StackModel&, const HexMeshArrays&);
} // namespace chipcae::mesh::cut
