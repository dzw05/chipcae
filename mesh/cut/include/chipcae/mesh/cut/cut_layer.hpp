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
    Local void_attribute = -1;
    std::vector<Index> cut_elem_gid;
    std::vector<Local> void_elem, small_subs;
    std::vector<std::array<Real, 3>> exterior_area_vector;
};
template <class MemSpace>
struct CutLayerView {
    View1<BodyId, MemSpace> ls_body;
    View1<Real, MemSpace> ls_phi; // body-major: ls_phi[body*nv+node]
    View1<Local, MemSpace> cut_elem, sub_off, sub_attribute, vq_off, bq_off, bq_bdr_attr, gp_face,
        void_elem, small_subs;
    View1<Real, MemSpace> sub_volfrac, vq_w, bq_w;
    std::array<View1<Real, MemSpace>, 3> vq_xi, bq_xi, bq_n;
    std::array<View1<Index, MemSpace>, 2> gp_elem_gid;
    View1<Index, MemSpace> cut_elem_gid;
    Local void_attribute = -1;
};
using DeviceCutLayerArrays = CutLayerView<DeviceSpace>;
template <class MemSpace = DeviceSpace>
CutLayerView<MemSpace> to_device(const CutLayerArrays& c) {
    CutLayerView<MemSpace> d;
#define K1_UPLOAD(field)                                                                           \
    d.field = upload_vector<typename decltype(c.field)::value_type, MemSpace>(c.field, #field)
    K1_UPLOAD(cut_elem);
    K1_UPLOAD(sub_off);
    K1_UPLOAD(sub_attribute);
    K1_UPLOAD(sub_volfrac);
    K1_UPLOAD(vq_off);
    K1_UPLOAD(vq_w);
    K1_UPLOAD(bq_off);
    K1_UPLOAD(bq_w);
    K1_UPLOAD(bq_bdr_attr);
    K1_UPLOAD(gp_face);
    K1_UPLOAD(cut_elem_gid);
    K1_UPLOAD(void_elem);
    K1_UPLOAD(small_subs);
#undef K1_UPLOAD
    for (int i = 0; i < 3; ++i) {
        d.vq_xi[i] = upload_vector<Real, MemSpace>(c.vq_xi[i], "vq_xi");
        d.bq_xi[i] = upload_vector<Real, MemSpace>(c.bq_xi[i], "bq_xi");
        d.bq_n[i] = upload_vector<Real, MemSpace>(c.bq_n[i], "bq_n");
    }
    for (int i = 0; i < 2; ++i)
        d.gp_elem_gid[i] = upload_vector<Index, MemSpace>(c.gp_elem_gid[i], "gp_elem_gid");
    std::vector<BodyId> bodies;
    std::vector<Real> phi;
    for (const auto& ls : c.level_sets) {
        bodies.push_back(ls.body);
        phi.insert(phi.end(), ls.phi.begin(), ls.phi.end());
    }
    d.ls_body = upload_vector<BodyId, MemSpace>(bodies, "ls_body");
    d.ls_phi = upload_vector<Real, MemSpace>(phi, "ls_phi");
    d.void_attribute = c.void_attribute;
    return d;
}
Expected<ActivationResolution> resolve_cut_activation(const StackModel&,
                                                      const HexMeshArrays&,
                                                      const CutLayerArrays&,
                                                      const std::vector<std::string>&);
struct GhostFaces {
    std::array<std::vector<Index>, 2> elem_gid;
    std::vector<Local> face; // Legacy MFEM local mesh-face index; not the global identity.
    std::vector<Index> face_gid;
    std::vector<Local> face_minus, face_plus, owner_rank;
    // elem_gid[0/1] are respectively gid_minus/gid_plus (strictly increasing).
};
class GhostPenaltyCache {
    std::vector<std::vector<Index>> masks_;
    HexMeshArrays mesh_; // Attribute-to-GID resolution for compatibility lookup(mask).

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
