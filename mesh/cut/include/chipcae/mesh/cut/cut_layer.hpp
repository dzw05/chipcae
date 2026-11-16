#pragma once
#include "chipcae/core/memory_space.hpp"
#include "chipcae/mesh/background/activation.hpp"
namespace chipcae::mesh::cut {
struct BodyLevelSet {
    BodyId body{};
    std::vector<Real> phi;
};
struct CutLayerArrays {
    std::vector<BodyLevelSet> level_sets;
    std::vector<Local> cut_elem, sub_off{0}, sub_attribute;
    std::vector<Real> sub_volfrac;
    std::vector<Local> vq_off{0}, bq_off{0};
    std::array<std::vector<Real>, 3> vq_xi, bq_xi, bq_n;
    std::vector<Real> vq_w, bq_w;
    std::vector<Local> bq_bdr_attr;
    std::array<std::vector<Index>, 2> gp_elem_gid;
    std::vector<Local> gp_face;
    Local void_attribute = 0;
    std::vector<Index> cut_elem_gid;
    std::vector<Local> void_elem, small_subs;
    std::vector<std::array<Real, 3>> exterior_area_vector;
};
struct DeviceBodyLevelSet {
    BodyId body;
    View1<Real, DeviceSpace> phi;
};
struct DeviceCutLayerArrays {
    std::vector<DeviceBodyLevelSet> level_sets;
    View1<Local, DeviceSpace> cut_elem, sub_off, sub_attribute, vq_off, bq_off, bq_bdr_attr,
        gp_face, void_elem, small_subs;
    View1<Real, DeviceSpace> sub_volfrac, vq_w, bq_w;
    std::array<View1<Real, DeviceSpace>, 3> vq_xi, bq_xi, bq_n;
    std::array<View1<Index, DeviceSpace>, 2> gp_elem_gid;
    View1<Index, DeviceSpace> cut_elem_gid;
    Local void_attribute = 0;
};
// Explicit upload; caller owns this snapshot and reuses it throughout solver steps.
DeviceCutLayerArrays to_device(const CutLayerArrays&);
Expected<ActivationResolution> resolve_cut_activation(const StackModel&,
                                                      const HexMeshArrays&,
                                                      const CutLayerArrays&,
                                                      const std::vector<std::string>&);
struct GhostFaces {
    std::array<std::vector<Index>, 2> elem_gid;
    std::vector<Local> face;
};
class GhostPenaltyCache {
    std::vector<ActivationMask> masks_;

public:
    std::vector<std::uint64_t> hashes;
    std::vector<Local> offsets{0};
    GhostFaces faces;
    static Expected<GhostPenaltyCache>
    prepare(const HexMeshArrays&, const CutLayerArrays&, std::vector<ActivationMask>);
    Expected<GhostFaces> lookup(ActivationMask) const;
    Expected<bool> set_face(Index a, Index b, Local face);
};
} // namespace chipcae::mesh::cut
