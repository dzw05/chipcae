#pragma once
#include "../../../preview/src/mesh_digest.hpp"
#include "chipcae/mesh/background/tensor_grid.hpp"
#include "chipcae/mesh/cut/region_level_set.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <numeric>
#include <set>
#include <stdexcept>
namespace sp5 {
using namespace chipcae::mesh;
inline void require(bool ok, const char* message) {
    if (!ok)
        throw std::runtime_error(message);
}
template <class F>
int run(F f) {
    try {
        f();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
inline StackModel stack() {
    StackModel s;
    s.id = "sp5";
    s.outline.box = {0, 0, 8, 8};
    Layer l;
    l.name = "void_layer";
    l.kind = "Explicit";
    l.thickness = 1;
    l.n_elem_min = 2;
    s.layers = {l};
    return s;
}
inline Inclusion box(std::string name, double a, double b, double c, double d) {
    return {name, "solid", name, {{a, b, c, d}, {}}};
}
inline Inclusion square(std::string name, double a, double b, double c, double d) {
    auto r = box(name, a, b, c, d);
    r.footprint.polygon = {{a, b}, {c, b}, {c, d}, {a, d}};
    return r;
}
inline HexMeshArrays grid(const StackModel& s, int n = 4, int ranks = 1) {
    GridOptions o;
    o.nx = n;
    o.ny = n;
    o.levels = 1;
    o.ranks = ranks;
    o.cell_budget = 1000000;
    auto result = build_tensor_grid(s, o);
    if (!result)
        throw std::runtime_error("background fixture: " + result.error().path + ": " +
                                 result.error().reason);
    return result.value();
}
// Full rectangular sampling grid deliberately independent of void-cell filtering.
inline HexMeshArrays samples(int n) {
    HexMeshArrays m;
    for (int k = 0; k < 3; ++k)
        for (int j = 0; j <= n; ++j)
            for (int i = 0; i <= n; ++i) {
                m.vertices.insert(m.vertices.end(), {8. * i / n, 8. * j / n, .5 * k});
                m.node_gid.push_back(static_cast<chipcae::Index>(m.node_gid.size()));
            }
    for (int i = 0; i <= n; ++i) {
        m.axes[0].push_back(8. * i / n);
        m.axes[1].push_back(8. * i / n);
    }
    m.axes[2] = {0, .5, 1};
    const int plane = (n + 1) * (n + 1);
    for (int k = 0; k < 2; ++k)
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i) {
                int a = k * plane + j * (n + 1) + i;
                m.hex.insert(m.hex.end(),
                             {a,
                              a + 1,
                              a + n + 2,
                              a + n + 1,
                              a + plane,
                              a + plane + 1,
                              a + plane + n + 2,
                              a + plane + n + 1});
                m.elem_gid.push_back(static_cast<chipcae::Index>(m.elem_gid.size()));
            }
    return m;
}
inline void bodies(HexMeshArrays& m, const StackModel& s) {
    int id = 7;
    for (const auto& l : s.layers)
        for (const auto& r : l.regions) {
            auto name = r.body.empty() ? r.name : r.body;
            if (!m.attr_table.bodies.contains(name))
                m.attr_table.bodies[name] = id++;
        }
}
inline double rect(double x, double y, double a, double b, double c, double d) {
    double qx = std::max(a - x, x - c), qy = std::max(b - y, y - d);
    return std::hypot(std::max(qx, 0.), std::max(qy, 0.)) + std::min(std::max(qx, qy), 0.);
}
inline std::vector<BodyLevelSet> levels(const StackModel& s, HexMeshArrays& m) {
    bodies(m, s);
    auto ls = region_level_sets(s, m, {});
    require(!ls.empty(), "polygon must produce BodyLevelSet");
    for (const auto& l : ls)
        require(l.phi.size() == m.node_gid.size(), "phi must cover every node");
    return ls;
}
// Marching squares: linearly interpolate crossings on each edge, then shoelace.
// Our convex rounded fixture and resolved ring have no ambiguous saddle cells.
inline double area(const std::vector<double>& phi, int n) {
    double result = 0, h = 8. / n;
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            std::array<std::array<double, 2>, 4> xy = {{{0, 0}, {h, 0}, {h, h}, {0, h}}};
            std::array<double, 4> v = {phi[j * (n + 1) + i],
                                       phi[j * (n + 1) + i + 1],
                                       phi[(j + 1) * (n + 1) + i + 1],
                                       phi[(j + 1) * (n + 1) + i]};
            std::vector<std::array<double, 2>> poly;
            for (int e = 0; e < 4; ++e) {
                int next = (e + 1) % 4;
                if (v[e] < 0)
                    poly.push_back(xy[e]);
                if ((v[e] < 0) != (v[next] < 0)) {
                    double t = v[e] / (v[e] - v[next]);
                    poly.push_back({xy[e][0] + t * (xy[next][0] - xy[e][0]),
                                    xy[e][1] + t * (xy[next][1] - xy[e][1])});
                }
            }
            double a = 0;
            for (std::size_t k = 0; k < poly.size(); ++k) {
                auto b = (k + 1) % poly.size();
                a += poly[k][0] * poly[b][1] - poly[b][0] * poly[k][1];
            }
            result += std::abs(a) * .5;
        }
    return result;
}
inline void compact(const HexMeshArrays& m) {
    std::set<Local> used(m.hex.begin(), m.hex.end());
    require(used.size() == m.node_gid.size(), "unused nodes must be removed");
    require(std::set<chipcae::Index>(m.node_gid.begin(), m.node_gid.end()).size() ==
                m.node_gid.size(),
            "node GIDs must be unique");
    require(std::set<chipcae::Index>(m.elem_gid.begin(), m.elem_gid.end()).size() ==
                m.elem_gid.size(),
            "element GIDs must be unique");
}
inline std::array<double, 3> center(const HexMeshArrays& m, std::size_t e) {
    std::array<double, 3> c{};
    for (int v = 0; v < 8; ++v)
        for (int d = 0; d < 3; ++d)
            c[d] += m.vertices[3 * m.hex[8 * e + v] + d] / 8;
    return c;
}
} // namespace sp5
