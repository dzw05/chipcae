#include "chipcae/solvers/fvm/grid.hpp"

namespace chipcae::solvers::fvm {
// Deliberate test-first stubs: no geometry or material behavior implemented.
Expected<FvGrid> build_grid(const mesh::HexMeshArrays&, const mesh::cut::CutLayerArrays*) {
    return mesh::Error{"fvm/grid", "NOT_IMPLEMENTED: FVM thermal grid (test-first stub)"};
}
std::vector<double> copper_fraction(const FvGrid&, const mesh::HexMeshArrays&) {
    return {};
}
} // namespace chipcae::solvers::fvm
