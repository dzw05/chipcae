#include "chipcae/mesh/cut/plane_cut.hpp"

#include "chipcae/mesh/background/activation.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>
namespace chipcae::mesh::cut {
namespace {
using V = std::array<Real, 3>;
struct SimplexPoint {
    std::array<Real, 4> l;
    Real w;
};
// Jacobi P_n^(alpha,0) on [-1,1], with its derivative.
std::pair<Real, Real> jacobi(int n, int alpha, Real x) {
    Real p = 1, dp = 0, q = 0, dq = 0;
    for (int k = 1; k <= n; ++k) {
        Real t, dt;
        if (k == 1) {
            t = (alpha + (alpha + 2) * x) / 2;
            dt = Real(alpha + 2) / 2;
        } else {
            Real h = 2 * k + alpha;
            Real a = (h - 1) * (h * (h - 2) * x + alpha * alpha);
            Real da = (h - 1) * h * (h - 2);
            Real b = 2 * (k + alpha - 1) * (k - 1) * h;
            Real c = 2 * k * (k + alpha) * (h - 2);
            t = (a * p - b * q) / c;
            dt = (da * p + a * dp - b * dq) / c;
        }
        q = p;
        dq = dp;
        p = t;
        dp = dt;
    }
    return {p, dp};
}
// Shifted Gauss-Jacobi: integrates (1-u)^alpha on [0,1]. alpha=0 is Legendre.
Expected<std::vector<std::pair<Real, Real>>> gauss01(int n, int alpha = 0) {
    std::vector<Real> bounds{-1};
    if (n > 1) {
        auto inner = gauss01(n - 1, alpha);
        if (!inner)
            return inner.error();
        for (auto [u, w] : inner.value())
            bounds.push_back(2 * u - 1);
    }
    bounds.push_back(1);
    std::vector<std::pair<Real, Real>> out;
    for (int i = 0; i < n; ++i) {
        Real lo = bounds[i], hi = bounds[i + 1], x = (lo + hi) / 2;
        Real plo = jacobi(n, alpha, lo).first;
        bool converged = false;
        for (int iteration = 0; iteration < 100; ++iteration) {
            auto [p, dp] = jacobi(n, alpha, x);
            Real dx = p / dp;
            if (std::abs(dx) <=
                4 * std::numeric_limits<Real>::epsilon() * std::max(Real(1), std::abs(x))) {
                x -= dx;
                converged = true;
                break;
            }
            if ((p > 0) == (plo > 0)) {
                lo = x;
                plo = p;
            } else
                hi = x;
            Real next = x - dx;
            x = next > lo && next < hi ? next : (lo + hi) / 2;
        }
        if (!converged)
            return Error{"cut/quadrature", "Gauss-Jacobi root did not converge in 100 iterations"};
        auto [p, dp] = jacobi(n, alpha, x);
        // The Jacobi normalization constant cancels the shift's 2^(alpha+1).
        out.push_back({(x + 1) / 2, 1 / ((1 - x * x) * dp * dp)});
    }
    return out;
}
Expected<std::vector<SimplexPoint>> tetra_rule(int degree) {
    int n = degree / 2 + 1;
    auto gu = gauss01(n, 2), gv = gauss01(n, 1), gz = gauss01(n);
    if (!gu)
        return gu.error();
    if (!gv)
        return gv.error();
    if (!gz)
        return gz.error();
    std::vector<SimplexPoint> out;
    for (auto [u, wu] : gu.value())
        for (auto [v, wv] : gv.value())
            for (auto [z, wz] : gz.value())
                out.push_back({{(1 - u) * (1 - v) * (1 - z), u, (1 - u) * v, (1 - u) * (1 - v) * z},
                               wu * wv * wz});
    return out;
}
struct TriPoint {
    std::array<Real, 3> l;
    Real w;
};
Expected<std::vector<TriPoint>> triangle_rule(int degree) {
    int n = degree / 2 + 1;
    auto gu = gauss01(n, 1), gv = gauss01(n);
    if (!gu)
        return gu.error();
    if (!gv)
        return gv.error();
    std::vector<TriPoint> out;
    for (auto [u, wu] : gu.value())
        for (auto [v, wv] : gv.value())
            out.push_back({{(1 - u) * (1 - v), u, (1 - u) * v}, wu * wv});
    return out;
}
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
std::map<BodyId, Local> compact_embedded_attributes(const HexMeshArrays& m,
                                                    const std::vector<Local>& used_bodies) {
    Local next = 7;
    for (const auto& [name, list] : m.face_sets)
        if (!name.ends_with("|void"))
            for (auto a : list)
                next = std::max(next, a + 1);
    std::set<BodyId> used(used_bodies.begin(), used_bodies.end());
    std::map<BodyId, Local> out;
    for (const auto& [name, body] : m.attr_table.bodies)
        if (used.contains(body))
            out[body] = next++;
    return out;
}
void register_embedded(HexMeshArrays& m, const CutLayerArrays& c) {
    for (const auto& [ref, attrs] : c.face_sets) {
        auto& list = m.face_sets[ref];
        for (auto attr : attrs) {
            if (std::find(list.begin(), list.end(), attr) == list.end())
                list.push_back(attr);
            m.attr_table.boundary_names[attr] = ref;
        }
    }
}
Expected<CutLayerArrays>
generate(const HexMeshArrays& m, const CutGeometry& g, const CutOptions& o) {
    if (!(o.snap_phi >= 0) || !(o.small_sub_vf >= 0 && o.small_sub_vf < 1) ||
        (o.quad_degree != 2 && o.quad_degree != 4 && o.quad_degree != 6 && o.quad_degree != 8))
        return Error{"cut/options", "invalid thresholds"};
    CutLayerArrays c;
    if (o.export_polyhedra)
        c.polyhedra.emplace();
    // Match the preview writer's stable, one-based layer-name order.
    std::map<std::string, Local> output_layers;
    if (o.export_polyhedra)
        for (const auto& attr : m.attr_table.entries)
            if (!output_layers.contains(attr.layer))
                output_layers[attr.layer] = static_cast<Local>(output_layers.size() + 1);
    std::vector<SimplexPoint> v_rule;
    std::vector<TriPoint> b_rule;
    if (o.quad_degree != 2) {
        auto volume_rule = tetra_rule(o.quad_degree);
        if (!volume_rule)
            return volume_rule.error();
        v_rule = std::move(volume_rule.value());
        auto boundary_rule = triangle_rule(o.quad_degree);
        if (!boundary_rule)
            return boundary_rule.error();
        b_rule = std::move(boundary_rule.value());
    }
    auto all_surface_rule = triangle_rule(o.quad_degree);
    if (!all_surface_rule)
        return all_surface_rule.error();
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
                std::array<V, 4> tet{center, a, b, d};
                if (o.quad_degree == 2) {
                    constexpr Real high = .58541019662496845446, low = .13819660112501051518;
                    for (int q = 0; q < 4; ++q) {
                        V point{};
                        for (int k = 0; k < 4; ++k)
                            point = add(point, mul(tet[k], q == k ? high : low));
                        point = xi(point);
                        for (int k = 0; k < 3; ++k)
                            c.vq_xi[k].push_back(point[k]);
                        c.vq_w.push_back(vol / (4 * jac));
                    }
                } else
                    for (const auto& q : v_rule) {
                        V point{};
                        for (int k = 0; k < 4; ++k)
                            point = add(point, mul(tet[k], q.l[k]));
                        point = xi(point);
                        for (int k = 0; k < 3; ++k)
                            c.vq_xi[k].push_back(point[k]);
                        c.vq_w.push_back(vol * q.w * 6 / jac);
                    }
                // Mesh owns quadrature on every cut face, including the
                // surviving pieces of the original Cartesian boundary faces.
                int side = -1;
                if (!f.embedded) {
                    for (int k = 0; k < 3; ++k)
                        if (std::abs(f.normal[k]) > .5)
                            side = 2 * k + (f.normal[k] > 0 ? 1 : 0);
                }
                for (const auto& q : all_surface_rule.value()) {
                    V point{};
                    for (int k = 0; k < 3; ++k)
                        point = add(point, mul(std::array<V, 3>{a, b, d}[k], q.l[k]));
                    point = xi(point);
                    for (int k = 0; k < 3; ++k)
                        c.sq_xi[k].push_back(point[k]);
                    c.sq_w.push_back(area * q.w * 2);
                    c.sq_side.push_back(side);
                }
                if (f.embedded) {
                    for (auto vertex : {a, b, d}) {
                        auto reference = xi(vertex);
                        c.eb_points.insert(c.eb_points.end(), vertex.begin(), vertex.end());
                        c.eb_xi.insert(c.eb_xi.end(), reference.begin(), reference.end());
                        c.eb_normals.insert(c.eb_normals.end(), f.normal.begin(), f.normal.end());
                        c.eb_parent_elem.push_back(static_cast<Local>(e));
                    }
                    if (o.quad_degree == 2)
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
                    else
                        for (const auto& q : b_rule) {
                            V point{};
                            for (int k = 0; k < 3; ++k)
                                point = add(point, mul(std::array<V, 3>{a, b, d}[k], q.l[k]));
                            point = xi(point);
                            for (int k = 0; k < 3; ++k) {
                                c.bq_xi[k].push_back(point[k]);
                                c.bq_n[k].push_back(f.normal[k]);
                            }
                            c.bq_w.push_back(area * q.w * 2);
                            c.bq_bdr_attr.push_back(body);
                        }
                } else
                    exterior = add(exterior, mul(f.normal, area));
            }
        if (c.polyhedra) {
            // Physical snap, independent of the normalized signed-distance test.
            Real snap = std::max(o.snap_phi,
                                 32 * std::numeric_limits<Real>::epsilon() *
                                     std::max({size[0], size[1], size[2]}));
            std::vector<V> points;
            std::vector<std::vector<Index>> rings;
            for (const auto& f : faces) {
                std::vector<Index> ring;
                for (auto p : f.p) {
                    auto it = std::find_if(
                        points.begin(), points.end(), [&](V q) { return norm(sub(p, q)) <= snap; });
                    Index id = it - points.begin();
                    if (it == points.end())
                        points.push_back(p);
                    if (std::find(ring.begin(), ring.end(), id) == ring.end())
                        ring.push_back(id);
                }
                V area{};
                for (std::size_t j = 1; j + 1 < ring.size(); ++j)
                    area = add(area,
                               cross(sub(points[ring[j]], points[ring[0]]),
                                     sub(points[ring[j + 1]], points[ring[0]])));
                if (ring.size() >= 3 && norm(area) / 2 >= snap * snap)
                    rings.push_back(std::move(ring));
            }
            Real signed_volume = 0;
            for (const auto& ring : rings)
                for (std::size_t j = 1; j + 1 < ring.size(); ++j)
                    signed_volume +=
                        dot(sub(points[ring[0]], center),
                            cross(sub(points[ring[j]], center), sub(points[ring[j + 1]], center))) /
                        6;
            std::map<std::pair<Index, Index>, int> edges;
            for (const auto& ring : rings)
                for (std::size_t j = 0; j < ring.size(); ++j)
                    ++edges[{ring[j], ring[(j + 1) % ring.size()]}];
            bool closed = true;
            for (const auto& [edge, count] : edges) {
                auto reverse = edges.find({edge.second, edge.first});
                closed &= count == 1 && reverse != edges.end() && reverse->second == 1;
            }
            if (!(rings.size() >= 4 && signed_volume > 0 && closed)) {
                std::ostringstream reason;
                reason << std::setprecision(std::numeric_limits<Real>::max_digits10)
                       << "elem_gid=" << m.elem_gid[e] << " rings=" << rings.size()
                       << " signed_volume=" << signed_volume << " closed=" << std::boolalpha
                       << closed;
                return Error{"cut/polyhedron", reason.str()};
            }
            {
                auto& out = *c.polyhedra;
                std::vector<Index> remap(points.size(), -1);
                for (const auto& ring : rings) {
                    for (auto id : ring) {
                        if (remap[id] < 0) {
                            remap[id] = out.points.size() / 3;
                            auto ref = xi(points[id]);
                            out.points.insert(
                                out.points.end(), points[id].begin(), points[id].end());
                            out.point_xi.insert(out.point_xi.end(), ref.begin(), ref.end());
                            out.point_parent_elem.push_back(static_cast<Local>(e));
                        }
                        out.face_conn.push_back(remap[id]);
                    }
                    out.poly_faces.push_back(out.face_off.size() - 1);
                    out.face_off.push_back(out.face_conn.size());
                }
                out.poly_off.push_back(out.poly_faces.size());
                const auto& attr = m.attr_table.entries[m.attribute[e] - 1];
                out.layer_id.push_back(output_layers.at(attr.layer));
                out.material_id.push_back(attr.material);
                out.body.push_back(body);
                out.cell_kind.push_back(1);
                out.parent_elem_gid.push_back(m.elem_gid[e]);
                out.volume.push_back(signed_volume);
                out.volfrac_ref.push_back(volume / jac);
            }
        }
        c.sub_volfrac.push_back(volume / jac);
        if (volume / jac < o.small_sub_vf)
            c.small_subs.push_back(static_cast<Local>(c.sub_attribute.size() - 1));
        c.sub_off.push_back(static_cast<Local>(c.sub_attribute.size()));
        c.sq_off.push_back(static_cast<Local>(c.sq_w.size()));
        c.vq_off.push_back(static_cast<Local>(c.vq_w.size()));
        c.bq_off.push_back(static_cast<Local>(c.bq_w.size()));
        c.exterior_area_vector.push_back(exterior);
    }
    auto eb = compact_embedded_attributes(m, c.bq_bdr_attr);
    for (auto& a : c.bq_bdr_attr)
        a = eb.at(a);
    for (const auto& [name, body] : m.attr_table.bodies)
        if (eb.contains(body))
            c.face_sets[canonical_interface("body:" + name, "void")] = {eb[body]};
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
    if (c.polyhedra)
        for (auto& e : c.polyhedra->point_parent_elem)
            e = map[e];
    for (auto& e : c.cut_elem)
        e = map[e];
    for (auto& e : c.eb_parent_elem)
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
                m.face_sets[name].push_back(it->second);
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
