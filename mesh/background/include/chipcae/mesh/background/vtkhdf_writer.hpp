#pragma once
#include "chipcae/core/communicator.hpp"
#include "hex_mesh_arrays.hpp"
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
} // namespace chipcae::mesh
