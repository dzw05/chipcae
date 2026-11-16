#include "chipcae/mesh/cut/cut_layer.hpp"

#include <algorithm>
#include <set>
namespace chipcae::mesh::cut {
namespace {
template <class T>
View1<T, DeviceSpace> upload(const std::vector<T>& source, const std::string& name) {
    View1<T, DeviceSpace> view(name, source.size());
#ifdef CHIPCAE_ENABLE_KOKKOS
    auto host = Kokkos::create_mirror_view(view);
    for (std::size_t i = 0; i < source.size(); ++i)
        host(i) = source[i];
    Kokkos::deep_copy(view, host);
#else
    for (std::size_t i = 0; i < source.size(); ++i)
        view(i) = source[i];
#endif
    return view;
}
ActivationMask canonical(ActivationMask mask) {
    std::sort(mask.begin(), mask.end());
    mask.erase(std::unique(mask.begin(), mask.end()), mask.end());
    return mask;
}
} // namespace
DeviceCutLayerArrays to_device(const CutLayerArrays& c) {
    DeviceCutLayerArrays d;
#define K1_UPLOAD(field) d.field = upload(c.field, #field)
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
        d.vq_xi[i] = upload(c.vq_xi[i], "vq_xi");
        d.bq_xi[i] = upload(c.bq_xi[i], "bq_xi");
        d.bq_n[i] = upload(c.bq_n[i], "bq_n");
    }
    for (int i = 0; i < 2; ++i)
        d.gp_elem_gid[i] = upload(c.gp_elem_gid[i], "gp_elem_gid");
    for (const auto& l : c.level_sets)
        d.level_sets.push_back({l.body, upload(l.phi, "phi")});
    d.void_attribute = c.void_attribute;
    return d;
}
Expected<ActivationResolution> resolve_cut_activation(const StackModel& s,
                                                      const HexMeshArrays& m,
                                                      const CutLayerArrays& c,
                                                      const std::vector<std::string>& refs) {
    return resolve_activation(s, m, &c, refs);
}

Expected<GhostPenaltyCache> GhostPenaltyCache::prepare(const HexMeshArrays& m,
                                                       const CutLayerArrays& c,
                                                       std::vector<ActivationMask> masks) {
    GhostPenaltyCache cache;
    std::set<Index> cut(c.cut_elem_gid.begin(), c.cut_elem_gid.end());
    struct Neighbor {
        Local a, b, face;
    };
    std::vector<Neighbor> pairs;
    constexpr int ids[6][4] = {
        {0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4}, {3, 7, 6, 2}, {0, 4, 7, 3}, {1, 2, 6, 5}};
    std::map<std::array<Local, 4>, std::pair<Local, Local>> faces;
    for (std::size_t e = 0; e < m.attribute.size(); ++e)
        for (Local f = 0; f < 6; ++f) {
            std::array<Local, 4> key;
            for (int j = 0; j < 4; ++j)
                key[j] = m.hex[8 * e + ids[f][j]];
            std::sort(key.begin(), key.end());
            auto [it, inserted] = faces.emplace(key, std::pair{static_cast<Local>(e), f});
            if (!inserted)
                pairs.push_back({it->second.first, static_cast<Local>(e), -1});
        }
    for (auto& mask : masks)
        mask = canonical(mask);
    std::sort(masks.begin(), masks.end(), [](const auto& a, const auto& b) {
        return mask_hash(a) < mask_hash(b);
    });
    for (const auto& mask : masks) {
        auto hash = mask_hash(mask);
        if (!cache.hashes.empty() && cache.hashes.back() == hash) {
            if (cache.masks_.back() != mask)
                return Error{"gp/cache", "hash collision"};
            continue;
        }
        cache.hashes.push_back(hash);
        cache.masks_.push_back(mask);
        for (auto p : pairs)
            if ((cut.contains(m.elem_gid[p.a]) || cut.contains(m.elem_gid[p.b])) &&
                std::binary_search(mask.begin(), mask.end(), m.attribute[p.a]) &&
                std::binary_search(mask.begin(), mask.end(), m.attribute[p.b])) {
                cache.faces.elem_gid[0].push_back(m.elem_gid[p.a]);
                cache.faces.elem_gid[1].push_back(m.elem_gid[p.b]);
                cache.faces.face.push_back(p.face);
            }
        cache.offsets.push_back(static_cast<Local>(cache.faces.face.size()));
    }
    return cache;
}
Expected<GhostFaces> GhostPenaltyCache::lookup(ActivationMask mask) const {
    mask = canonical(mask);
    auto h = mask_hash(mask);
    auto it = std::lower_bound(hashes.begin(), hashes.end(), h);
    if (it == hashes.end() || *it != h)
        return Error{"gp/cache", "unbudgeted activation mask"};
    auto i = static_cast<std::size_t>(it - hashes.begin());
    if (masks_[i] != mask)
        return Error{"gp/cache", "hash collision"};
    GhostFaces out;
    for (Local k = offsets[i]; k < offsets[i + 1]; ++k) {
        out.elem_gid[0].push_back(faces.elem_gid[0][k]);
        out.elem_gid[1].push_back(faces.elem_gid[1][k]);
        out.face.push_back(faces.face[k]);
    }
    return out;
}
Expected<bool> GhostPenaltyCache::set_face(Index a, Index b, Local face) {
    bool found = false;
    for (std::size_t i = 0; i < faces.face.size(); ++i)
        if ((faces.elem_gid[0][i] == a && faces.elem_gid[1][i] == b) ||
            (faces.elem_gid[0][i] == b && faces.elem_gid[1][i] == a)) {
            faces.face[i] = face;
            found = true;
        }
    if (!found)
        return Error{"gp/face", "GID pair not budgeted"};
    return true;
}
} // namespace chipcae::mesh::cut
