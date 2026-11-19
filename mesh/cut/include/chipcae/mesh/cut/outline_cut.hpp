#pragma once
#include "plane_cut.hpp"

namespace chipcae::mesh::cut {
Expected<std::optional<CutLayerArrays>>
apply_outline_cut(HexMeshArrays&, const Footprint&, const CutOptions&);
Expected<std::optional<CutLayerArrays>>
apply_outline_cut(HexMeshArrays&, const Footprint&, const CutOptions&, const CutGeometry&);
} // namespace chipcae::mesh::cut
