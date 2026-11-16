#include "chipcae/mesh/cut/cut_layer.hpp"

#include <algorithm>
#include <set>
namespace chipcae::mesh::cut {
Expected<ActivationResolution> resolve_cut_activation(const StackModel& s,
                                                      const HexMeshArrays& m,
                                                      const CutLayerArrays& c,
                                                      const std::vector<std::string>& refs) {
    return resolve_activation(s, m, &c, refs);
}
Expected<ActivationResolution> resolve_cut_activation(const StackModel& s,
                                                      HexMeshArrays& m,
                                                      const CutLayerArrays& c,
                                                      const std::vector<std::string>& refs) {
    return resolve_activation(s, m, &c, refs);
}

Expected<GhostPenaltyCache> GhostPenaltyCache::prepare(const HexMeshArrays& m,
                                                       const CutLayerArrays& c,
                                                       std::vector<ActivationMask> masks) {
    GhostPenaltyCache cache;
    cache.mesh_ = m;
    std::set<Index> cut(c.cut_elem_gid.begin(), c.cut_elem_gid.end());
    struct Neighbor {
        Local a, b, fa, fb;
        Index gid;
    };
    std::vector<Neighbor> pairs;
    constexpr int ids[6][4] = {
        {0, 3, 2, 1}, {0, 1, 5, 4}, {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7}, {4, 5, 6, 7}};
    std::map<std::array<Local, 4>, std::pair<Local, Local>> faces;
    for (std::size_t e = 0; e < m.attribute.size(); ++e)
        for (Local f = 0; f < 6; ++f) {
            std::array<Local, 4> key;
            for (int j = 0; j < 4; ++j)
                key[j] = m.hex[8 * e + ids[f][j]];
            std::sort(key.begin(), key.end());
            auto [it, inserted] = faces.emplace(key, std::pair{static_cast<Local>(e), f});
            if (!inserted) {
                auto a = it->second.first, b = static_cast<Local>(e), fa = it->second.second,
                     fb = f;
                if (m.elem_gid[a] > m.elem_gid[b]) {
                    std::swap(a, b);
                    std::swap(fa, fb);
                }
                pairs.push_back({a, b, fa, fb, m.elem_gid[a] * 6 + fa});
            }
        }
    std::vector<std::vector<Index>> sets;
    for (const auto& mask : masks)
        sets.push_back(activated_gids(m, mask));
    std::sort(sets.begin(), sets.end(), [](const auto& a, const auto& b) {
        return mask_hash(a) < mask_hash(b);
    });
    for (const auto& ids : sets) {
        auto hash = mask_hash(ids);
        if (!cache.hashes.empty() && cache.hashes.back() == hash) {
            if (cache.masks_.back() != ids)
                return Error{"gp/cache", "hash collision"};
            continue;
        }
        cache.hashes.push_back(hash);
        cache.masks_.push_back(ids);
        for (auto p : pairs)
            if ((cut.contains(m.elem_gid[p.a]) || cut.contains(m.elem_gid[p.b])) &&
                std::binary_search(ids.begin(), ids.end(), m.elem_gid[p.a]) &&
                std::binary_search(ids.begin(), ids.end(), m.elem_gid[p.b])) {
                cache.faces.elem_gid[0].push_back(m.elem_gid[p.a]);
                cache.faces.elem_gid[1].push_back(m.elem_gid[p.b]);
                cache.faces.face.push_back(-1);
                cache.faces.face_gid.push_back(p.gid);
                cache.faces.face_minus.push_back(p.fa);
                cache.faces.face_plus.push_back(p.fb);
                cache.faces.owner_rank.push_back(m.partitioning[p.a]);
            }
        cache.offsets.push_back(static_cast<Local>(cache.faces.face.size()));
    }
    return cache;
}
Expected<GhostFaces> GhostPenaltyCache::lookup(ActivationMask mask) const {
    for (auto a : mask)
        if (a < 1 || static_cast<std::size_t>(a) > mesh_.attr_table.entries.size())
            return Error{"gp/cache", "invalid attribute mask"};
    auto ids = activated_gids(mesh_, mask);
    auto h = mask_hash(ids);
    auto it = std::lower_bound(hashes.begin(), hashes.end(), h);
    if (it == hashes.end() || *it != h)
        return Error{"gp/cache", "unbudgeted activation mask"};
    auto i = static_cast<std::size_t>(it - hashes.begin());
    if (masks_[i] != ids)
        return Error{"gp/cache", "hash collision"};
    GhostFaces out;
    for (Local k = offsets[i]; k < offsets[i + 1]; ++k) {
        out.elem_gid[0].push_back(faces.elem_gid[0][k]);
        out.elem_gid[1].push_back(faces.elem_gid[1][k]);
        out.face.push_back(faces.face[k]);
        out.face_gid.push_back(faces.face_gid[k]);
        out.face_minus.push_back(faces.face_minus[k]);
        out.face_plus.push_back(faces.face_plus[k]);
        out.owner_rank.push_back(faces.owner_rank[k]);
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
