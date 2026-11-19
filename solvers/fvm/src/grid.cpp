#include "chipcae/solvers/fvm/grid.hpp"

#include "mfd_local.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <map>
#include <numeric>
#include <set>
#include <sstream>
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
struct Plane {
    V n{}, point{};
};
long double dot(V a, V b) {
    long double s = 0;
    for (int d = 0; d < 3; ++d)
        s += static_cast<long double>(a[d]) * b[d];
    return s;
}
V difference(V a, V b) {
    for (int d = 0; d < 3; ++d)
        a[d] -= b[d];
    return a;
}
Expected<bool> validate_cut(const mesh::HexMeshArrays& m, const mesh::cut::CutLayerArrays& c) {
    const auto n = c.cut_elem.size();
    auto offsets = [](const auto& a, std::size_t records, std::size_t length) {
        if (a.size() != records + 1 || a.front() != 0 || a.back() < 0 ||
            static_cast<std::size_t>(a.back()) != length)
            return false;
        for (std::size_t i = 1; i < a.size(); ++i)
            if (a[i] < a[i - 1])
                return false;
        return true;
    };
    if (!offsets(c.sub_off, n, c.sub_attribute.size()) ||
        c.sub_attribute.size() != c.sub_volfrac.size() || !offsets(c.vq_off, n, c.vq_w.size()) ||
        !offsets(c.sq_off, n, c.sq_w.size()) || c.sq_side.size() != c.sq_w.size() ||
        c.cut_elem_gid.size() != n || c.exterior_area_vector.size() != n)
        return bad("inconsistent cut arrays/offsets");
    for (int d = 0; d < 3; ++d)
        if (c.vq_xi[d].size() != c.vq_w.size() || c.sq_xi[d].size() != c.sq_w.size())
            return bad("inconsistent quadrature coordinates");
    if (c.eb_points.size() != 3 * c.eb_parent_elem.size() ||
        c.eb_normals.size() != c.eb_points.size())
        return bad("inconsistent embedded triangles");
    for (auto e : c.eb_parent_elem)
        if (e < 0 || static_cast<std::size_t>(e) >= m.attribute.size())
            return bad("invalid embedded parent");
    for (auto x : c.eb_points)
        if (!std::isfinite(x))
            return bad("nonfinite embedded vertex");
    for (auto x : c.eb_normals)
        if (!std::isfinite(x))
            return bad("nonfinite embedded normal");
    std::set<mesh::Local> parents;
    for (std::size_t i = 0; i < n; ++i) {
        auto e = c.cut_elem[i];
        if (e < 0 || static_cast<std::size_t>(e) >= m.attribute.size() ||
            !parents.insert(e).second || c.cut_elem_gid[i] != m.elem_gid[e])
            return bad("invalid cut parent");
        if (c.sub_off[i + 1] - c.sub_off[i] != 1)
            return mesh::Error{"fvm/grid",
                               "NOT_IMPLEMENTED: ambiguous parent quadrature ownership gid=" +
                                   std::to_string(m.elem_gid[e])};
        long double total = 0;
        for (auto s = c.sub_off[i]; s < c.sub_off[i + 1]; ++s) {
            auto a = c.sub_attribute[s];
            if (a <= 0 || static_cast<std::size_t>(a) > m.attr_table.entries.size() ||
                !(c.sub_volfrac[s] > 0) || !std::isfinite(c.sub_volfrac[s]))
                return bad("invalid subcell gid=" + std::to_string(m.elem_gid[e]));
            const auto& record = m.attr_table.entries[a - 1];
            auto l = m.attr_table.layers.find(record.layer);
            if (l == m.attr_table.layers.end() || l->second <= 0 ||
                static_cast<std::size_t>(l->second) > m.copper_layer_of_layer.size())
                return bad("invalid subcell layer");
            total += c.sub_volfrac[s];
        }
        if (total > 1 + 1e-14L)
            return bad("overfull subcell fractions gid=" + std::to_string(m.elem_gid[e]));
    }
    for (auto e : c.void_elem)
        if (e < 0 || static_cast<std::size_t>(e) >= m.attribute.size() || !parents.insert(e).second)
            return bad("invalid/duplicate void parent");
    for (auto s : c.small_subs)
        if (s < 0 || static_cast<std::size_t>(s) >= c.sub_attribute.size())
            return bad("invalid small_subs index");
    for (std::size_t q = 0; q < c.vq_w.size(); ++q) {
        if (c.vq_w[q] < 0 || !std::isfinite(c.vq_w[q]))
            return bad("invalid volume weight");
        for (int d = 0; d < 3; ++d)
            if (!std::isfinite(c.vq_xi[d][q]))
                return bad("invalid volume coordinate");
    }
    for (std::size_t q = 0; q < c.sq_w.size(); ++q) {
        if (c.sq_w[q] < 0 || !std::isfinite(c.sq_w[q]) || c.sq_side[q] < -1 || c.sq_side[q] > 5)
            return bad("invalid surface quadrature");
        for (int d = 0; d < 3; ++d)
            if (!std::isfinite(c.sq_xi[d][q]))
                return bad("invalid surface coordinate");
    }
    return true;
}
Expected<std::vector<FvFace>> cut_faces(const mesh::cut::CutLayerArrays& cut,
                                        std::size_t record,
                                        const Box& b,
                                        std::size_t element,
                                        double& volume,
                                        V& center) {
    const auto jac = static_cast<long double>(b.h[0]) * b.h[1] * b.h[2];
    long double vol = 0, first[3]{};
    for (auto q = cut.vq_off[record]; q < cut.vq_off[record + 1]; ++q) {
        auto w = jac * cut.vq_w[q];
        vol += w;
        for (int d = 0; d < 3; ++d)
            first[d] += w * cut.vq_xi[d][q] * b.h[d];
    }
    const auto expected = jac * cut.sub_volfrac[cut.sub_off[record]];
    const auto prefix = "gid=" + std::to_string(cut.cut_elem_gid[record]) + " subcell=0 ";
    if (!(vol > 0) || !std::isfinite(vol) || std::abs(vol - expected) > 1e-12L * expected)
        return bad(prefix + "volume mismatch");
    volume = static_cast<double>(vol);
    for (int d = 0; d < 3; ++d)
        center[d] = b.lo[d] + static_cast<double>(first[d] / vol);
    const auto tolerance = 1e-12L * std::min({b.h[0], b.h[1], b.h[2]});
    std::vector<Plane> planes;
    for (std::size_t i = 0; i < cut.eb_parent_elem.size(); ++i)
        if (cut.eb_parent_elem[i] == static_cast<std::int64_t>(element)) {
            Plane plane;
            for (int d = 0; d < 3; ++d) {
                plane.n[d] = cut.eb_normals[3 * i + d];
                plane.point[d] = cut.eb_points[3 * i + d];
            }
            auto length = std::sqrt(dot(plane.n, plane.n));
            if (!(length > 0))
                return bad(prefix + "zero embedded normal");
            for (auto& x : plane.n)
                x = static_cast<double>(x / length);
            bool found = false;
            for (const auto& p : planes)
                if (dot(plane.n, p.n) > 1 - 1e-12L &&
                    std::abs(dot(p.n, difference(plane.point, p.point))) <= tolerance)
                    found = true;
            if (!found)
                planes.push_back(plane);
        }
    std::vector<long double> area(6 + planes.size());
    std::vector<std::array<long double, 3>> moment(area.size());
    for (auto q = cut.sq_off[record]; q < cut.sq_off[record + 1]; ++q) {
        if (cut.sq_w[q] == 0)
            continue;
        V x{}, offset{};
        for (int d = 0; d < 3; ++d) {
            offset[d] = b.h[d] * cut.sq_xi[d][q];
            x[d] = b.lo[d] + offset[d];
        }
        auto s = cut.sq_side[q];
        std::size_t group = 0;
        if (s >= 0) {
            group = static_cast<std::size_t>(s);
            if (std::abs(static_cast<long double>(offset[s / 2]) - (s % 2 ? b.h[s / 2] : 0)) >
                tolerance)
                return bad(prefix + "axial quadrature off plane");
        } else {
            long double best = INFINITY;
            for (std::size_t i = 0; i < planes.size(); ++i) {
                auto distance = std::abs(dot(planes[i].n, difference(x, planes[i].point)));
                if (distance < best) {
                    best = distance;
                    group = 6 + i;
                }
            }
            if (best > tolerance)
                return bad(prefix + "embedded point has no triangle plane");
        }
        area[group] += cut.sq_w[q];
        for (int d = 0; d < 3; ++d)
            moment[group][d] += static_cast<long double>(cut.sq_w[q]) * offset[d];
    }
    std::vector<FvFace> out;
    V exterior{};
    for (std::size_t i = 0; i < area.size(); ++i)
        if (area[i] > 0) {
            FvFace f;
            f.area = static_cast<double>(area[i]);
            f.axis = i < 6 ? static_cast<int>(i / 2) : -1;
            f.side = i < 6 ? static_cast<int>(i) : 6;
            for (int d = 0; d < 3; ++d)
                f.centroid[d] = b.lo[d] + static_cast<double>(moment[i][d] / area[i]);
            if (i < 6) {
                f.area_vector[i / 2] = (i % 2 ? 1 : -1) * f.area;
                exterior[i / 2] += f.area_vector[i / 2];
            } else
                for (int d = 0; d < 3; ++d)
                    f.area_vector[d] = planes[i - 6].n[d] * f.area;
            out.push_back(f);
        }
    const double parent_area = std::max({b.h[0] * b.h[1], b.h[0] * b.h[2], b.h[1] * b.h[2]});
    for (int d = 0; d < 3; ++d)
        if (!std::isfinite(cut.exterior_area_vector[record][d]) ||
            std::abs(exterior[d] - cut.exterior_area_vector[record][d]) > 1e-12 * parent_area)
            return bad(prefix + "exterior area-vector mismatch");
    return out;
}
} // namespace
Expected<FvGrid> build_grid(const mesh::HexMeshArrays& m, const mesh::cut::CutLayerArrays* cut) {
    auto checked = boxes(m);
    if (!checked)
        return checked.error();
    const auto& bs = checked.value();
    if (cut) {
        auto valid = validate_cut(m, *cut);
        if (!valid)
            return valid.error();
    }
    FvGrid g;
    g.axes = m.axes;
    g.nz = m.axes[2].size() - 1;
    std::vector<std::int64_t> cut_record(bs.size(), -1),
        sub_cv(cut ? cut->sub_attribute.size() : 0, -1);
    std::vector<bool> is_void(bs.size());
    if (cut) {
        for (auto e : cut->void_elem)
            is_void[e] = true;
        for (std::size_t i = 0; i < cut->cut_elem.size(); ++i)
            cut_record[cut->cut_elem[i]] = static_cast<std::int64_t>(i);
    }
    std::vector<std::size_t> order(bs.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(
        order.begin(), order.end(), [&](auto a, auto b) { return m.elem_gid[a] < m.elem_gid[b]; });
    std::map<std::array<std::size_t, 4>, std::size_t> pending;
    for (auto e : order) {
        if (is_void[e])
            continue;
        const auto& b = bs[e];
        const auto record = cut_record[e];
        const auto c = static_cast<std::int64_t>(g.cv_volume.size());
        const auto attribute =
            record >= 0 ? cut->sub_attribute[cut->sub_off[record]] : m.attribute[e];
        g.cv_elem.push_back(static_cast<std::int64_t>(e));
        g.cv_elem_gid.push_back(m.elem_gid[e]);
        g.cv_attribute.push_back(attribute);
        g.cv_layer.push_back(m.attr_table.layers.at(m.attr_table.entries[attribute - 1].layer));
        g.cv_kind.push_back(record >= 0 ? 1 : 0);
        g.cv_rank.push_back(m.partitioning[e]);
        g.cv_subcell.push_back(0);
        g.constituent_elem.push_back(static_cast<std::int64_t>(e));
        g.constituent_gid.push_back(m.elem_gid[e]);
        g.constituent_subcell.push_back(0);
        g.constituent_off.push_back(c + 1);
        double volume = b.h[0] * b.h[1] * b.h[2];
        V center{};
        for (int d = 0; d < 3; ++d)
            center[d] = b.lo[d] + b.h[d] / 2;
        const double fraction = record >= 0 ? cut->sub_volfrac[cut->sub_off[record]] : 1;
        std::vector<FvFace> faces;
        if (record >= 0) {
            auto created = cut_faces(*cut, static_cast<std::size_t>(record), b, e, volume, center);
            if (!created)
                return created.error();
            faces = std::move(created.value());
            sub_cv[cut->sub_off[record]] = c;
        } else
            for (int s = 0; s < 6; ++s) {
                int d = s / 2, t1 = (d + 1) % 3, t2 = (d + 2) % 3;
                FvFace f;
                f.axis = d;
                f.side = s;
                f.area = b.h[t1] * b.h[t2];
                f.centroid = center;
                f.centroid[d] = m.axes[d][b.ijk[d] + s % 2];
                f.area_vector[d] = (s % 2 ? 1 : -1) * f.area;
                faces.push_back(f);
            }
        g.cv_volume.push_back(volume);
        g.cv_volfrac.push_back(fraction);
        g.cv_centroid.push_back(center);
        auto layer = static_cast<std::size_t>(g.cv_layer.back() - 1);
        auto cl = m.copper_layer_of_layer[layer];
        const auto ncol = (m.axes[0].size() - 1) * (m.axes[1].size() - 1);
        double phi = cl < 0 ? 0
                            : m.copper_phi[static_cast<std::size_t>(cl) * ncol +
                                           static_cast<std::size_t>(m.elem_gid[e]) / g.nz];
        g.cv_copper_fraction.push_back(std::min(1., phi / fraction));
        for (auto f : faces) {
            f.lo = c;
            f.parent_elem = static_cast<std::int64_t>(e);
            if (f.axis < 0) {
                g.faces.push_back(f);
                continue;
            }
            const int d = f.axis, t1 = (d + 1) % 3, t2 = (d + 2) % 3;
            std::array<std::size_t, 4> key{
                static_cast<std::size_t>(d), b.ijk[d] + f.side % 2, b.ijk[t1], b.ijk[t2]};
            auto found = pending.find(key);
            if (found != pending.end()) {
                auto& old = g.faces[found->second];
                const auto& other = bs[static_cast<std::size_t>(g.cv_elem[old.lo])];
                const auto prefix =
                    "opening mismatch gids=" + std::to_string(g.cv_elem_gid[old.lo]) + "," +
                    std::to_string(m.elem_gid[e]) + " ";
                if (std::abs(old.area - f.area) > 1e-12 * std::max(old.area, f.area))
                    return bad(prefix + "area");
                for (int a = 0; a < 3; ++a)
                    if (std::abs(old.centroid[a] - f.centroid[a]) >
                        1e-12 * std::min(b.h[a], other.h[a])) {
                        std::ostringstream reason;
                        reason << std::setprecision(17) << prefix << "centroid axis=" << a
                               << " first=" << old.centroid[a] << " second=" << f.centroid[a]
                               << " difference=" << std::abs(old.centroid[a] - f.centroid[a])
                               << " tolerance=" << 1e-12 * std::min(b.h[a], other.h[a]);
                        return bad(reason.str());
                    }
                old.hi = c;
                old.side = -1;
                pending.erase(found);
            } else {
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
    if (cut && !cut->small_subs.empty()) {
        const auto nc = g.cv_volume.size();
        std::vector<std::size_t> parent(nc);
        std::iota(parent.begin(), parent.end(), 0);
        auto find = [&](std::size_t a) {
            while (parent[a] != a)
                a = parent[a];
            return a;
        };
        auto identity = [&](std::size_t a) { return std::pair{g.cv_elem_gid[a], g.cv_subcell[a]}; };
        auto small = cut->small_subs;
        std::sort(small.begin(), small.end(), [&](auto a, auto b) {
            return identity(static_cast<std::size_t>(sub_cv[a])) <
                   identity(static_cast<std::size_t>(sub_cv[b]));
        });
        small.erase(std::unique(small.begin(), small.end()), small.end());
        for (auto s : small) {
            auto from = find(static_cast<std::size_t>(sub_cv[s]));
            std::map<std::size_t, long double> openings;
            for (const auto& f : g.faces)
                if (f.hi >= 0) {
                    auto a = find(static_cast<std::size_t>(f.lo)),
                         b = find(static_cast<std::size_t>(f.hi));
                    if (a == b)
                        continue;
                    if (a == from)
                        openings[b] += f.area;
                    else if (b == from)
                        openings[a] += f.area;
                }
            std::size_t best = nc;
            long double maximum = -1;
            for (const auto& [neighbor, area] : openings) {
                const auto& a = m.attr_table.entries[g.cv_attribute[from] - 1];
                const auto& b = m.attr_table.entries[g.cv_attribute[neighbor] - 1];
                if (a.material != b.material || a.layer != b.layer)
                    continue;
                if (area > maximum || (area == maximum && identity(neighbor) < identity(best))) {
                    maximum = area;
                    best = neighbor;
                }
            }
            if (best == nc)
                return mesh::Error{
                    "fvm/grid",
                    "NOT_IMPLEMENTED: listed merge has no same-material/layer neighbor gid=" +
                        std::to_string(g.cv_elem_gid[from])};
            if (identity(from) < identity(best))
                parent[best] = from;
            else
                parent[from] = best;
        }
        std::map<std::size_t, std::vector<std::size_t>> groups;
        for (std::size_t c = 0; c < nc; ++c)
            groups[find(c)].push_back(c);
        FvGrid merged;
        merged.axes = g.axes;
        merged.nz = g.nz;
        std::vector<std::int64_t> renumber(nc);
        for (const auto& [rep, members] : groups) {
            auto id = static_cast<std::int64_t>(merged.cv_volume.size());
            long double vol = 0, phi = 0, pvol = 0;
            std::array<long double, 3> first{};
            for (auto c : members) {
                renumber[c] = id;
                vol += g.cv_volume[c];
                phi += static_cast<long double>(g.cv_volume[c]) * g.cv_copper_fraction[c];
                for (int d = 0; d < 3; ++d)
                    first[d] += static_cast<long double>(g.cv_volume[c]) * g.cv_centroid[c][d];
                const auto& b = bs[static_cast<std::size_t>(g.cv_elem[c])];
                pvol += static_cast<long double>(b.h[0]) * b.h[1] * b.h[2];
                merged.constituent_elem.push_back(g.cv_elem[c]);
                merged.constituent_gid.push_back(g.cv_elem_gid[c]);
                merged.constituent_subcell.push_back(g.cv_subcell[c]);
            }
            merged.constituent_off.push_back(
                static_cast<std::int64_t>(merged.constituent_elem.size()));
            merged.cv_elem.push_back(g.cv_elem[rep]);
            merged.cv_elem_gid.push_back(g.cv_elem_gid[rep]);
            merged.cv_subcell.push_back(g.cv_subcell[rep]);
            merged.cv_rank.push_back(g.cv_rank[rep]);
            merged.cv_attribute.push_back(g.cv_attribute[rep]);
            merged.cv_layer.push_back(g.cv_layer[rep]);
            merged.cv_kind.push_back(members.size() > 1 ? 2 : g.cv_kind[rep]);
            merged.cv_volume.push_back(static_cast<double>(vol));
            merged.cv_volfrac.push_back(static_cast<double>(vol / pvol));
            merged.cv_copper_fraction.push_back(static_cast<double>(phi / vol));
            V center{};
            for (int d = 0; d < 3; ++d)
                center[d] = static_cast<double>(first[d] / vol);
            merged.cv_centroid.push_back(center);
        }
        for (auto f : g.faces) {
            if (f.hi >= 0 && renumber[f.lo] == renumber[f.hi])
                continue;
            f.lo = renumber[f.lo];
            if (f.hi >= 0)
                f.hi = renumber[f.hi];
            merged.faces.push_back(f);
        }
        csr(merged);
        g = std::move(merged);
        for (std::size_t c = 0; c < g.cv_volume.size(); ++c) {
            auto gate = detail::geometry_defect(g, c);
            if (!gate)
                return gate.error();
            g.max_identity_defect = std::max(g.max_identity_defect, gate.value());
        }
    }
    return g;
}
std::vector<double> copper_fraction(const FvGrid& g, const mesh::HexMeshArrays&) {
    return g.cv_copper_fraction;
}
} // namespace chipcae::solvers::fvm
