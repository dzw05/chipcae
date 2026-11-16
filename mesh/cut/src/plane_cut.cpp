#include "chipcae/mesh/cut/plane_cut.hpp"

#include "chipcae/mesh/background/activation.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <tuple>
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
Local embedded_attribute(const HexMeshArrays& m, BodyId body) {
    // Body IDs are never boundary attributes. Reserve a range above aligned patches.
    Local next = 7;
    for (const auto& [name, list] : m.face_sets)
        if (!name.ends_with("|void"))
            for (auto a : list)
                next = std::max(next, a + 1);
    return next + body;
}
void register_embedded(HexMeshArrays& m, const CutLayerArrays& c) {
    for (const auto& [name, body] : m.attr_table.bodies) {
        auto attr_test = embedded_attribute(m, body);
        if (std::find(c.bq_bdr_attr.begin(), c.bq_bdr_attr.end(), attr_test) == c.bq_bdr_attr.end())
            continue;
        auto ref = canonical_interface("body:" + name, "void");
        auto attr = embedded_attribute(m, body);
        m.face_sets[ref] = {attr};
        m.attr_table.boundary_names[attr] = ref;
    }
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
                        c.bq_bdr_attr.push_back(embedded_attribute(m, body));
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
    for (const auto& [name, body] : m.attr_table.bodies) {
        auto attr = embedded_attribute(m, body);
        if (std::find(c.bq_bdr_attr.begin(), c.bq_bdr_attr.end(), attr) != c.bq_bdr_attr.end())
            c.face_sets[canonical_interface("body:" + name, "void")] = {attr};
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
    register_embedded(m, c);
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
    if (c.void_attribute >= 1)
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
    std::map<std::array<Local, 4>, std::tuple<std::array<Local, 4>, int, Local>> boundary;
    for (std::size_t e = 0; e < m.attribute.size(); ++e)
        for (int f = 0; f < 6; ++f) {
            std::array<Local, 4> v;
            for (int j = 0; j < 4; ++j)
                v[j] = m.hex[e * 8 + fi[f][j]];
            auto key = v;
            std::sort(key.begin(), key.end());
            auto it = boundary.find(key);
            if (it == boundary.end())
                boundary[key] = {v, 1, static_cast<Local>(e)};
            else
                ++std::get<1>(it->second);
        }
    auto quad_area = [&](const std::array<Local, 4>& q) {
        Real area = 1;
        std::array<Real, 3> span{};
        for (int d = 0; d < 3; ++d) {
            Real lo = m.vertices[3 * q[0] + d], hi = lo;
            for (auto v : q) {
                lo = std::min(lo, m.vertices[3 * v + d]);
                hi = std::max(hi, m.vertices[3 * v + d]);
            }
            span[d] = hi - lo;
        }
        for (auto v : span)
            if (v > 0)
                area *= v;
        return area;
    };
    auto body_name = [&](BodyId id) {
        for (const auto& [name, body] : m.attr_table.bodies)
            if (body == id)
                return name;
        return std::string{};
    };
    Local next_attr = 1;
    for (auto a : m.bdr_attribute)
        next_attr = std::max(next_attr, a + 1);
    for (const auto& [name, list] : m.face_sets)
        for (auto a : list)
            next_attr = std::max(next_attr, a + 1);
    std::map<BodyId, Local> exposed_attr;
    for (auto [key, rec] : boundary)
        if (std::get<1>(rec) == 1) {
            m.bdr_quad.insert(m.bdr_quad.end(), std::get<0>(rec).begin(), std::get<0>(rec).end());
            if (original_boundary.contains(key)) {
                m.bdr_attribute.push_back(original_boundary[key]);
                continue;
            }
            auto elem = std::get<2>(rec);
            auto body = m.attr_table.entries[m.attribute[elem] - 1].body;
            auto name = canonical_interface("body:" + body_name(body), "void");
            auto [it, inserted] = exposed_attr.emplace(body, next_attr);
            if (inserted) {
                ++next_attr;
                m.face_sets[name] = {it->second};
                m.attr_table.boundary_names[it->second] = name;
                HexMeshArrays::InterfaceReport report;
                report.name = name;
                fill_interface_sides(report, m);
                m.preparation_report.push_back(report);
            }
            m.bdr_attribute.push_back(it->second);
            for (auto& r : m.preparation_report)
                if (r.name == name) {
                    ++r.aligned_faces;
                    r.aligned_area += quad_area(std::get<0>(rec));
                }
        }
    // Retain aligned internal patches only when both adjacent cells survive.
    for (auto [key, rec] : boundary)
        if (std::get<1>(rec) == 2 && original_boundary.contains(key)) {
            m.bdr_quad.insert(m.bdr_quad.end(), std::get<0>(rec).begin(), std::get<0>(rec).end());
            m.bdr_attribute.push_back(original_boundary[key]);
        }
    // Compact vertices transactionally, preserving their original global identities.
    std::vector<Local> remap(m.node_gid.size(), -1);
    for (auto v : m.hex)
        remap[v] = 0;
    HexMeshArrays nodes;
    for (std::size_t v = 0; v < remap.size(); ++v)
        if (remap[v] == 0) {
            remap[v] = static_cast<Local>(nodes.node_gid.size());
            nodes.vertices.insert(
                nodes.vertices.end(), m.vertices.begin() + 3 * v, m.vertices.begin() + 3 * v + 3);
            nodes.node_gid.push_back(m.node_gid[v]);
            nodes.node_layer.push_back(m.node_layer[v]);
            nodes.node_column.push_back(m.node_column[v]);
        }
    for (auto& v : m.hex)
        v = remap[v];
    for (auto& v : m.bdr_quad)
        v = remap[v];
    for (auto& ls : c.level_sets) {
        std::vector<Real> phi;
        for (std::size_t v = 0; v < remap.size(); ++v)
            if (remap[v] >= 0)
                phi.push_back(ls.phi[v]);
        ls.phi = std::move(phi);
    }
    m.vertices = std::move(nodes.vertices);
    m.node_gid = std::move(nodes.node_gid);
    m.node_layer = std::move(nodes.node_layer);
    m.node_column = std::move(nodes.node_column);
    m.attr_frame.assign(9 * m.attr_table.entries.size(), 0);
    for (std::size_t a = 0; a < m.attr_table.entries.size(); ++a)
        for (int d = 0; d < 3; ++d)
            m.attr_frame[9 * a + 4 * d] = 1;
    ActivationMask active;
    for (std::size_t a = 1; a <= m.attr_table.entries.size(); ++a)
        if (static_cast<Local>(a) != c.void_attribute)
            active.push_back(static_cast<Local>(a));
    auto gp = GhostPenaltyCache::prepare(m, c, {active});
    if (!gp)
        return gp.error();
    c.gp_elem_gid = gp.value().faces.elem_gid;
    c.gp_face = gp.value().faces.face;
    c.gp_face_gid = gp.value().faces.face_gid;
    c.gp_face_minus = gp.value().faces.face_minus;
    c.gp_face_plus = gp.value().faces.face_plus;
    c.gp_owner_rank = gp.value().faces.owner_rank;
    for (const auto& [name, list] : c.face_sets) {
        HexMeshArrays::InterfaceReport* report = nullptr;
        for (auto& r : m.preparation_report)
            if (r.name == name)
                report = &r;
        if (!report) {
            HexMeshArrays::InterfaceReport added;
            added.name = name;
            m.preparation_report.push_back(added);
            fill_interface_sides(m.preparation_report.back(), m);
            report = &m.preparation_report.back();
        }
        for (std::size_t q = 0; q < c.bq_w.size(); ++q)
            if (std::find(list.begin(), list.end(), c.bq_bdr_attr[q]) != list.end())
                report->cut_area += c.bq_w[q];
        for (std::size_t sub = 0; sub < c.sub_attribute.size(); ++sub)
            if (c.bq_off[sub + 1] > c.bq_off[sub] &&
                std::find(list.begin(), list.end(), c.bq_bdr_attr[c.bq_off[sub]]) != list.end())
                ++report->cut_faces;
    }
    mesh = std::move(m);
    return result;
}
} // namespace chipcae::mesh::cut
