#include "chipcae/mesh/cut/plane_cut.hpp"

#include <algorithm>
#include <cmath>
#include <set>
namespace chipcae::mesh::cut {
namespace {
using V = std::array<Real, 3>;
using Polygon = std::vector<V>;
V add(V a, V b) {
    for (int i = 0; i < 3; ++i)
        a[i] += b[i];
    return a;
}
V sub(V a, V b) {
    for (int i = 0; i < 3; ++i)
        a[i] -= b[i];
    return a;
}
V mul(V a, Real s) {
    for (auto& v : a)
        v *= s;
    return a;
}
Real dot(V a, V b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
V cross(V a, V b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
Real norm(V a) {
    return std::sqrt(dot(a, a));
}
struct Face {
    Polygon p;
    V normal;
    bool embedded = false;
};
std::vector<Face> clip(std::vector<Face> faces, V n, Real d, Real snap) {
    std::vector<Face> out;
    Polygon cap;
    auto phi = [&](V p) {
        auto f = dot(n, p) - d;
        return std::abs(f) <= snap ? Real(0) : f;
    };
    bool negative = false, positive = false;
    for (const auto& f : faces)
        for (auto p : f.p) {
            negative |= phi(p) < 0;
            positive |= phi(p) > 0;
        }
    if (!positive)
        return faces;
    if (!negative)
        return {};
    for (const auto& face : faces) {
        Polygon p;
        for (std::size_t i = 0; i < face.p.size(); ++i) {
            V a = face.p[i], b = face.p[(i + 1) % face.p.size()];
            Real pa = phi(a), pb = phi(b);
            if (pa <= 0)
                p.push_back(a);
            if ((pa < 0 && pb > 0) || (pa > 0 && pb < 0)) {
                auto v = add(a, mul(sub(b, a), pa / (pa - pb)));
                p.push_back(v);
                cap.push_back(v);
            }
            if (pa == 0)
                cap.push_back(a);
        }
        if (p.size() >= 3)
            out.push_back({p, face.normal, face.embedded});
    }
    Polygon unique;
    for (auto p : cap) {
        bool seen = false;
        for (auto q : unique)
            if (norm(sub(p, q)) <= snap)
                seen = true;
        if (!seen)
            unique.push_back(p);
    }
    if (unique.size() >= 3) {
        V center{};
        for (auto p : unique)
            center = add(center, p);
        center = mul(center, 1 / Real(unique.size()));
        V u = cross(n, std::abs(n[0]) < .9 ? V{1, 0, 0} : V{0, 1, 0});
        u = mul(u, 1 / norm(u));
        V v = cross(n, u);
        std::sort(unique.begin(), unique.end(), [&](V a, V b) {
            a = sub(a, center);
            b = sub(b, center);
            return std::atan2(dot(a, v), dot(a, u)) < std::atan2(dot(b, v), dot(b, u));
        });
        out.push_back({unique, n, true});
    }
    return out;
}
Expected<CutLayerArrays>
generate(const HexMeshArrays& m, const CutGeometry& g, const CutOptions& o) {
    if (!(o.snap_phi >= 0) || !(o.vf_min >= 0 && o.vf_min < 1))
        return Error{"cut/options", "invalid thresholds"};
    CutLayerArrays c;
    std::vector<Plane> planes = g.planes;
    for (auto& p : planes) {
        Real length = norm(p.n);
        if (!(length > 0) || !std::isfinite(length) || !std::isfinite(p.d))
            return Error{"cut/plane", "nonfinite or zero normal"};
        p.n = mul(p.n, 1 / length);
        p.d /= length;
        BodyLevelSet ls;
        ls.body = p.body;
        for (std::size_t i = 0; i < m.vertices.size(); i += 3)
            ls.phi.push_back(dot(p.n, {m.vertices[i], m.vertices[i + 1], m.vertices[i + 2]}) - p.d);
        auto existing = std::find_if(c.level_sets.begin(), c.level_sets.end(), [&](const auto& v) {
            return v.body == ls.body;
        });
        if (existing == c.level_sets.end())
            c.level_sets.push_back(std::move(ls));
        else
            for (std::size_t i = 0; i < ls.phi.size(); ++i)
                existing->phi[i] = std::max(existing->phi[i], ls.phi[i]);
    }
    constexpr int ids[6][4] = {
        {0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4}, {3, 7, 6, 2}, {0, 4, 7, 3}, {1, 2, 6, 5}};
    const V normals[6] = {{0, 0, -1}, {0, 0, 1}, {0, -1, 0}, {0, 1, 0}, {-1, 0, 0}, {1, 0, 0}};
    for (std::size_t e = 0; e < m.attribute.size(); ++e) {
        std::array<V, 8> vertices;
        for (int j = 0; j < 8; ++j)
            for (int a = 0; a < 3; ++a)
                vertices[j][a] = m.vertices[3 * m.hex[8 * e + j] + a];
        V lo = vertices[0], size = sub(vertices[6], lo);
        Real jac = size[0] * size[1] * size[2];
        if (!(size[0] > 0 && size[1] > 0 && size[2] > 0))
            return Error{"cut/mesh", "positive affine box required"};
        std::vector<Face> faces;
        for (int f = 0; f < 6; ++f) {
            Polygon p;
            for (int j : ids[f])
                p.push_back(vertices[j]);
            faces.push_back({p, normals[f], false});
        }
        auto body = m.attr_table.entries[static_cast<std::size_t>(m.attribute[e] - 1)].body;
        for (const auto& p : planes)
            if (p.body == 0 || p.body == body)
                faces = clip(std::move(faces), p.n, p.d, o.snap_phi);
        if (faces.empty()) {
            c.void_elem.push_back(static_cast<Local>(e));
            continue;
        }
        bool embedded = false;
        for (const auto& f : faces)
            embedded |= f.embedded;
        if (!embedded)
            continue;
        V center{};
        std::size_t count = 0;
        for (const auto& f : faces)
            for (auto p : f.p) {
                center = add(center, p);
                ++count;
            }
        center = mul(center, 1 / Real(count));
        c.cut_elem.push_back(static_cast<Local>(e));
        c.cut_elem_gid.push_back(m.elem_gid[e]);
        c.sub_attribute.push_back(m.attribute[e]);
        auto xi = [&](V p) {
            for (int a = 0; a < 3; ++a)
                p[a] = (p[a] - lo[a]) / size[a];
            return p;
        };
        Real volume = 0;
        V exterior{};
        for (const auto& f : faces)
            for (std::size_t j = 1; j + 1 < f.p.size(); ++j) {
                V a = f.p[0], b = f.p[j], d = f.p[j + 1];
                Real area = norm(cross(sub(b, a), sub(d, a))) / 2;
                Real vol = std::abs(dot(sub(a, center), cross(sub(b, center), sub(d, center)))) / 6;
                volume += vol;
                constexpr Real high = .58541019662496845446, low = .13819660112501051518;
                std::array<V, 4> tet{center, a, b, d};
                for (int q = 0; q < 4; ++q) {
                    V point{};
                    for (int k = 0; k < 4; ++k)
                        point = add(point, mul(tet[k], q == k ? high : low));
                    point = xi(point);
                    for (int k = 0; k < 3; ++k)
                        c.vq_xi[k].push_back(point[k]);
                    c.vq_w.push_back(vol / (4 * jac));
                }
                if (f.embedded) {
                    for (int q = 0; q < 3; ++q) {
                        V point = xi(add(mul(add(a, add(b, d)), Real(1) / 6),
                                         mul(q == 0   ? a
                                             : q == 1 ? b
                                                      : d,
                                             Real(1) / 2)));
                        for (int k = 0; k < 3; ++k) {
                            c.bq_xi[k].push_back(point[k]);
                            c.bq_n[k].push_back(f.normal[k]);
                        }
                        c.bq_w.push_back(area / 3);
                        c.bq_bdr_attr.push_back(body);
                    }
                } else
                    exterior = add(exterior, mul(f.normal, area));
            }
        c.sub_volfrac.push_back(volume / jac);
        if (volume / jac < o.vf_min)
            c.small_subs.push_back(static_cast<Local>(c.sub_attribute.size() - 1));
        c.sub_off.push_back(static_cast<Local>(c.sub_attribute.size()));
        c.vq_off.push_back(static_cast<Local>(c.vq_w.size()));
        c.bq_off.push_back(static_cast<Local>(c.bq_w.size()));
        c.exterior_area_vector.push_back(exterior);
    }
    return c;
}
} // namespace
Expected<CutLayerArrays>
build_cut_layer(const HexMeshArrays& m, const CutGeometry& g, const CutOptions& o) {
    return generate(m, g, o);
}
Expected<CutLayerArrays>
build_cut_layer(HexMeshArrays& mesh, const CutGeometry& g, const CutOptions& o) {
    auto result = generate(mesh, g, o);
    if (!result)
        return result.error();
    auto& c = result.value();
    HexMeshArrays m = mesh;
    std::set<Local> empty(c.void_elem.begin(), c.void_elem.end());
    std::vector<Local> map(m.attribute.size(), -1);
    m.hex.clear();
    m.attribute.clear();
    m.elem_gid.clear();
    m.partitioning.clear();
    m.pending_cut.clear();
    if (o.keep_void && !empty.empty()) {
        c.void_attribute = static_cast<Local>(m.attr_table.entries.size() + 1);
        m.attr_table.entries.push_back({"void", "", 0, 0});
    }
    for (std::size_t e = 0; e < mesh.attribute.size(); ++e)
        if (o.keep_void || !empty.contains(static_cast<Local>(e))) {
            map[e] = static_cast<Local>(m.attribute.size());
            m.hex.insert(m.hex.end(), mesh.hex.begin() + 8 * e, mesh.hex.begin() + 8 * (e + 1));
            m.attribute.push_back(empty.contains(static_cast<Local>(e)) ? c.void_attribute
                                                                        : mesh.attribute[e]);
            m.elem_gid.push_back(mesh.elem_gid[e]);
            m.partitioning.push_back(mesh.partitioning[e]);
        }
    for (auto& e : c.cut_elem)
        e = map[e];
    if (o.keep_void) {
        for (auto& e : c.void_elem)
            e = map[e];
    } else
        c.void_elem.clear();
    for (auto e : mesh.pending_cut)
        if (map[e] >= 0)
            m.pending_cut.push_back(map[e]);
    std::vector<Local> attrmap(m.attr_table.entries.size() + 1);
    std::vector<AttributeRecord> records;
    for (std::size_t a = 1; a < attrmap.size(); ++a)
        if (std::find(m.attribute.begin(), m.attribute.end(), static_cast<Local>(a)) !=
            m.attribute.end()) {
            records.push_back(m.attr_table.entries[a - 1]);
            attrmap[a] = static_cast<Local>(records.size());
        }
    for (auto& a : m.attribute)
        a = attrmap[a];
    for (auto& a : c.sub_attribute)
        a = attrmap[a];
    if (c.void_attribute)
        c.void_attribute = attrmap[c.void_attribute];
    for (auto& [ref, list] : m.attr_table.references) {
        std::vector<Local> kept;
        for (auto a : list)
            if (attrmap[a])
                kept.push_back(attrmap[a]);
        list = std::move(kept);
    }
    m.attr_table.entries = std::move(records);
    // Rebuild exposed quadrilaterals; trimmed exterior pieces remain in cut quadrature.
    std::map<std::array<Local, 4>, Local> original_boundary;
    for (std::size_t i = 0; i < mesh.bdr_attribute.size(); ++i) {
        std::array<Local, 4> key;
        std::copy_n(mesh.bdr_quad.begin() + 4 * i, 4, key.begin());
        std::sort(key.begin(), key.end());
        original_boundary[key] = mesh.bdr_attribute[i];
    }
    m.bdr_quad.clear();
    m.bdr_attribute.clear();
    constexpr int fi[6][4] = {
        {0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4}, {3, 7, 6, 2}, {0, 4, 7, 3}, {1, 2, 6, 5}};
    std::map<std::array<Local, 4>, std::pair<std::array<Local, 4>, int>> boundary;
    for (std::size_t e = 0; e < m.attribute.size(); ++e)
        for (int f = 0; f < 6; ++f) {
            std::array<Local, 4> v;
            for (int j = 0; j < 4; ++j)
                v[j] = m.hex[e * 8 + fi[f][j]];
            auto key = v;
            std::sort(key.begin(), key.end());
            auto it = boundary.find(key);
            if (it == boundary.end())
                boundary[key] = {v, 1};
            else
                ++it->second.second;
        }
    for (auto [key, v] : boundary)
        if (v.second == 1) {
            m.bdr_quad.insert(m.bdr_quad.end(), v.first.begin(), v.first.end());
            m.bdr_attribute.push_back(original_boundary.contains(key) ? original_boundary[key] : 7);
        }
    ActivationMask active;
    for (std::size_t a = 1; a <= m.attr_table.entries.size(); ++a)
        if (static_cast<Local>(a) != c.void_attribute)
            active.push_back(static_cast<Local>(a));
    auto gp = GhostPenaltyCache::prepare(m, c, {active});
    if (!gp)
        return gp.error();
    c.gp_elem_gid = gp.value().faces.elem_gid;
    c.gp_face = gp.value().faces.face;
    mesh = std::move(m);
    return result;
}
} // namespace chipcae::mesh::cut
