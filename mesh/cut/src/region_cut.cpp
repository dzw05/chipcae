#include "chipcae/mesh/cut/region_cut.hpp"

namespace chipcae::mesh::cut {
Expected<CutGeometry> region_cut_planes(const StackModel&, const HexMeshArrays&) {
    // Test-first link stub only. Geometry and validation are intentionally absent.
    return Error{"CUT_UNSUPPORTED", "region_cut_planes: not implemented (SP5b test-first stub)"};
}
} // namespace chipcae::mesh::cut
