#pragma once
#include "chipcae/core/communicator.hpp"
#include "hex_mesh_arrays.hpp"

#include <memory>
namespace mfem {
class Mesh;
class ParMesh;
} // namespace mfem
namespace chipcae::mesh::cut {
struct CutLayerArrays;
class GhostPenaltyCache;
} // namespace chipcae::mesh::cut
namespace chipcae::mesh {
class MfemAdapter {
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit MfemAdapter(std::unique_ptr<Impl>);

public:
    ~MfemAdapter();
    MfemAdapter(MfemAdapter&&) noexcept;
    MfemAdapter& operator=(MfemAdapter&&) noexcept;
    // Takes ownership of array storage; serial Mesh borrows the vertex buffer.
    static Expected<MfemAdapter> create(HexMeshArrays, const Communicator&);
    mfem::Mesh& serial();
    mfem::Mesh& local();
#ifdef CHIPCAE_ENABLE_MPI
    mfem::ParMesh& parallel();
#endif
    const std::vector<Index>& elem_gid() const;
    const HexMeshArrays& arrays() const;
    Expected<bool> backfill_gp(cut::CutLayerArrays&, cut::GhostPenaltyCache&);
};
} // namespace chipcae::mesh
