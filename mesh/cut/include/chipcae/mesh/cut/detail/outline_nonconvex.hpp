#pragma once

#include "chipcae/mesh/cut/outline_cut.hpp"

namespace chipcae::mesh::cut::detail {
Expected<std::optional<CutLayerArrays>> apply_outline_cut_general(HexMeshArrays&,
                                                                  const Footprint&,
                                                                  const CutOptions&,
                                                                  const CutGeometry& extra);
} // namespace chipcae::mesh::cut::detail
