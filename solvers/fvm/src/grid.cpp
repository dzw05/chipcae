#include "chipcae/solvers/fvm/grid.hpp"

#include "mfd_local.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <tuple>
namespace chipcae::solvers::fvm {
namespace {
using V = std::array<double, 3>;
using I = std::array<std::size_t, 3>;
mesh::Error bad(const std::string& s) {
    return {"fvm/grid", "INTERNAL: " + s};
}
struct Box {
    V lo{}, h{};
    I ijk{};
};
Expected<std::vector<Box>> boxes(const mesh::HexMeshArrays& m) {
    const auto ne = m.attribute.size();
    if (m.hex.size() != 8 * ne || m.elem_gid.size() != ne || m.partitioning.size() != ne ||
        m.vertices.size() % 3)
        return bad("inconsistent element arrays");
    for (const auto& a : m.axes) {
        if (a.size() < 2)
            return bad("axis has fewer than two coordinates");
        for (std::size_t i = 0; i < a.size(); ++i)
            if (!std::isfinite(a[i]) || (i && !(a[i] > a[i - 1])))
                return bad("nonpositive or nonfinite axis interval");
    }
    for (auto x : m.vertices)
        if (!std::isfinite(x))
            return bad("nonfinite vertex");
    const auto ncol = (m.axes[0].size() - 1) * (m.axes[1].size() - 1), nz = m.axes[2].size() - 1;
    int ncopper = 0;
    for (auto l : m.copper_layer_of_layer) {
        if (l < -1)
            return bad("invalid copper layer");
        ncopper = std::max(ncopper, l + 1);
    }
    if (m.copper_phi.size() != static_cast<std::size_t>(ncopper) * ncol)
        return bad("invalid copper map length");
    for (auto f : m.copper_phi)
        if (!std::isfinite(f) || f < 0 || f > 1)
            return bad("invalid copper fraction");
    std::vector<Box> out(ne);
    std::set<I> occupied;
    std::set<std::int64_t> gids;
    for (std::size_t e = 0; e < ne; ++e) {
        const auto prefix = "gid=" + std::to_string(m.elem_gid[e]) + " ";
        if (m.attribute[e] <= 0 ||
            static_cast<std::size_t>(m.attribute[e]) > m.attr_table.entries.size() ||
            m.partitioning[e] < 0 || m.elem_gid[e] < 0 ||
            static_cast<std::size_t>(m.elem_gid[e]) / nz >= ncol ||
            !gids.insert(m.elem_gid[e]).second)
            return bad(prefix + "invalid attribute, owner or gid");
        const auto& name = m.attr_table.entries[m.attribute[e] - 1].layer;
        auto layer = m.attr_table.layers.find(name);
        if (layer == m.attr_table.layers.end() || layer->second <= 0 ||
            static_cast<std::size_t>(layer->second) > m.copper_layer_of_layer.size())
            return bad(prefix + "invalid one-based layer mapping");
        auto& b = out[e];
        V hi{};
        for (int d = 0; d < 3; ++d) {
            b.lo[d] = INFINITY;
            hi[d] = -INFINITY;
        }
        for (int j = 0; j < 8; ++j) {
            auto v = m.hex[8 * e + j];
            if (v < 0 || static_cast<std::size_t>(v) >= m.vertices.size() / 3)
                return bad(prefix + "invalid vertex index");
            for (int d = 0; d < 3; ++d) {
                b.lo[d] = std::min(b.lo[d], m.vertices[3 * v + d]);
                hi[d] = std::max(hi[d], m.vertices[3 * v + d]);
            }
        }
        for (int d = 0; d < 3; ++d) {
            auto it = std::lower_bound(m.axes[d].begin(), m.axes[d].end(), b.lo[d]);
            if (it == m.axes[d].end() || *it != b.lo[d] || it + 1 == m.axes[d].end() ||
                *(it + 1) != hi[d])
                return bad(prefix + "hex does not span an axis interval");
            b.ijk[d] = static_cast<std::size_t>(it - m.axes[d].begin());
            b.h[d] = hi[d] - b.lo[d];
        }
        std::set<int> corners;
        for (int j = 0; j < 8; ++j) {
            int bits = 0;
            auto v = m.hex[8 * e + j];
            for (int d = 0; d < 3; ++d) {
                auto x = m.vertices[3 * v + d];
                if (x == hi[d])
                    bits |= 1 << d;
                else if (x != b.lo[d])
                    return bad(prefix + "non-Cartesian hex");
            }
            corners.insert(bits);
        }
        if (corners.size() != 8 || !occupied.insert(b.ijk).second)
            return bad(prefix + "duplicate or degenerate hex");
    }
    return out;
}
void csr(FvGrid& g) {
    std::vector<std::vector<std::int64_t>> lists(g.cv_volume.size());
    for (std::size_t f = 0; f < g.faces.size(); ++f) {
        lists[g.faces[f].lo].push_back(static_cast<std::int64_t>(f));
        if (g.faces[f].hi >= 0)
            lists[g.faces[f].hi].push_back(static_cast<std::int64_t>(f));
    }
    g.cv_face_off = {0};
    g.cv_faces.clear();
    for (const auto& list : lists) {
        g.cv_faces.insert(g.cv_faces.end(), list.begin(), list.end());
        g.cv_face_off.push_back(static_cast<std::int64_t>(g.cv_faces.size()));
    }
}
} // namespace
Expected<FvGrid> build_grid(const mesh::HexMeshArrays& m, const mesh::cut::CutLayerArrays* cut) {
    auto checked = boxes(m);
    if (!checked)
        return checked.error();
    const auto& bs = checked.value();
    if (cut && (!cut->cut_elem.empty() || !cut->void_elem.empty()))
        return mesh::Error{"fvm/grid", "NOT_IMPLEMENTED: cut geometry"};
    FvGrid g;
    g.axes = m.axes;
    g.nz = m.axes[2].size() - 1;
    // Key is axis, axis-plane index, and the two tangential interval indices.
    std::map<std::array<std::size_t, 4>, std::size_t> pending;
    for (std::size_t e = 0; e < bs.size(); ++e) {
        const auto& b = bs[e];
        const auto c = static_cast<std::int64_t>(g.cv_volume.size());
        g.cv_elem.push_back(static_cast<std::int64_t>(e));
        g.cv_elem_gid.push_back(m.elem_gid[e]);
        g.cv_attribute.push_back(m.attribute[e]);
        g.cv_layer.push_back(
            m.attr_table.layers.at(m.attr_table.entries[m.attribute[e] - 1].layer));
        g.cv_kind.push_back(0);
        g.cv_rank.push_back(m.partitioning[e]);
        g.cv_subcell.push_back(0);
        g.constituent_elem.push_back(static_cast<std::int64_t>(e));
        g.constituent_gid.push_back(m.elem_gid[e]);
        g.constituent_subcell.push_back(0);
        g.constituent_off.push_back(c + 1);
        g.cv_volume.push_back(b.h[0] * b.h[1] * b.h[2]);
        g.cv_volfrac.push_back(1);
        V center{};
        for (int d = 0; d < 3; ++d)
            center[d] = b.lo[d] + b.h[d] / 2;
        g.cv_centroid.push_back(center);
        auto layer = static_cast<std::size_t>(g.cv_layer.back() - 1);
        auto cl = m.copper_layer_of_layer[layer];
        const auto ncol = (m.axes[0].size() - 1) * (m.axes[1].size() - 1);
        g.cv_copper_fraction.push_back(
            cl < 0 ? 0
                   : m.copper_phi[static_cast<std::size_t>(cl) * ncol +
                                  static_cast<std::size_t>(m.elem_gid[e]) / g.nz]);
        for (int s = 0; s < 6; ++s) {
            int d = s / 2, t1 = (d + 1) % 3, t2 = (d + 2) % 3;
            std::array<std::size_t, 4> key{
                static_cast<std::size_t>(d), b.ijk[d] + s % 2, b.ijk[t1], b.ijk[t2]};
            auto found = pending.find(key);
            if (found != pending.end()) {
                auto& f = g.faces[found->second];
                f.hi = c;
                f.side = -1;
                pending.erase(found);
            } else {
                FvFace f;
                f.lo = c;
                f.axis = d;
                f.side = s;
                f.area = b.h[t1] * b.h[t2];
                f.centroid = center;
                f.centroid[d] = m.axes[d][key[1]];
                f.area_vector[d] = (s % 2 ? 1 : -1) * f.area;
                pending.emplace(key, g.faces.size());
                g.faces.push_back(f);
            }
        }
    }
    csr(g);
    for (std::size_t c = 0; c < g.cv_volume.size(); ++c) {
        auto gate = detail::geometry_defect(g, c);
        if (!gate)
            return gate.error();
        g.max_identity_defect = std::max(g.max_identity_defect, gate.value());
    }
    return g;
}
std::vector<double> copper_fraction(const FvGrid& g, const mesh::HexMeshArrays&) {
    return g.cv_copper_fraction;
}
} // namespace chipcae::solvers::fvm
