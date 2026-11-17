#pragma once
#include "chipcae/core/communicator.hpp"
#include "hex_mesh_arrays.hpp"

#include <array>
#include <vector>
namespace chipcae::geometry {
struct Defect;
}
namespace chipcae::mesh::cut {
struct CutLayerArrays;
}
namespace chipcae::mesh {
struct VtkhdfFiles {
    std::filesystem::path grid, embedded_boundary;
};
// Global preparation arrays are replicated. One collective file per step;
// serial HDF5 uses rank zero. Per-rank output is an explicit debug option.
Expected<VtkhdfFiles> write_vtkhdf(const std::filesystem::path& stem,
                                   const HexMeshArrays&,
                                   const cut::CutLayerArrays*,
                                   const Communicator&,
                                   bool debug_per_rank = false);
Expected<std::filesystem::path>
write_defect_points_vtkhdf(const std::filesystem::path& path,
                           const std::vector<geometry::Defect>& defects,
                           const std::vector<std::array<Real, 3>>& points);
} // namespace chipcae::mesh
