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
// stem.rankN.vtkhdf + stem.rankN.eb.vtkhdf; independent per-rank files.
Expected<VtkhdfFiles> write_vtkhdf(const std::filesystem::path& stem,
                                   const HexMeshArrays&,
                                   const cut::CutLayerArrays*,
                                   const Communicator&);
} // namespace chipcae::mesh
