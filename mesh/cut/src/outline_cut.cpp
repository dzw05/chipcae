#include "chipcae/mesh/cut/outline_cut.hpp"

namespace chipcae::mesh::cut {
// Test-first link stubs: deliberately provide no clipping or validation behavior.
Expected<std::optional<CutLayerArrays>>
apply_outline_cut(HexMeshArrays&, const Footprint&, const CutOptions&) {
    return std::optional<CutLayerArrays>{};
}
Expected<std::optional<CutLayerArrays>>
apply_outline_cut(HexMeshArrays&, const Footprint&, const CutOptions&, const CutGeometry&) {
    return std::optional<CutLayerArrays>{};
}
} // namespace chipcae::mesh::cut
