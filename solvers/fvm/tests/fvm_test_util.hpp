#pragma once

#include "chipcae/material/copper_laminate.hpp"
#include "chipcae/mesh/background/sfc.hpp"
#include "chipcae/mesh/cut/plane_cut.hpp"
#include "chipcae/solvers/fvm/thermal.hpp"
#include "mfd_local.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <numeric>
#include <petscsys.h>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace fvm_test {
namespace fv = chipcae::solvers::fvm;
namespace mesh = chipcae::mesh;
using Vec = std::array<double, 3>;
using Ring = std::vector<Vec>;
using Poly = std::vector<Ring>;

inline void require(bool ok, const std::string& what) {
    if (!ok)
        throw std::runtime_error(what);
}
inline void near(double got, double want, double abs, double rel, const std::string& what) {
    if (!std::isfinite(got) || !std::isfinite(want) ||
        std::abs(got - want) > abs + rel * std::abs(want)) {
        std::ostringstream out;
        out << std::setprecision(17) << what << ": got " << got << ", expected " << want
            << ", abs_tol=" << abs << ", rel_tol=" << rel;
        throw std::runtime_error(out.str());
    }
}
template <class T>
T take(mesh::Expected<T> result, const std::string& what) {
    if (!result)
        throw std::runtime_error(what + ": " + result.error().path + ": " + result.error().reason);
    return std::move(result.value());
}
template <class T>
void rejected(const mesh::Expected<T>& result,
              const std::string& context,
              const std::string& diagnostic = {}) {
    require(!result, context + ": must reject input");
    const auto& error = result.error();
    require(!error.path.empty() && !error.reason.empty(),
            context + ": error needs path and reason");
    require(error.reason.find("NOT_IMPLEMENTED") == std::string::npos,
            context + ": validation is missing: " + error.reason);
    if (!diagnostic.empty())
        require(error.reason.find(diagnostic) != std::string::npos,
                context + ": missing diagnostic '" + diagnostic + "': " + error.reason);
}

struct Suite {
    int rank = 0, passed = 0, failed = 0;
    Suite() { MPI_Comm_rank(MPI_COMM_WORLD, &rank); }
    template <class F>
    void run(const std::string& name, F&& body) {
        std::string error;
        try {
            body();
        } catch (const std::exception& e) {
            error = e.what();
        }
        int local = error.empty() ? 0 : 1, any = 0;
        MPI_Allreduce(&local, &any, 1, MPI_INT, MPI_MAX, MPI_COMM_WORLD);
        if (any) {
            ++failed;
            if (local)
                std::cerr << "FAIL [rank " << rank << "] " << name << ": " << error << '\n';
        } else {
            ++passed;
            if (rank == 0)
                std::cout << "PASS " << name << '\n';
        }
    }
    int finish() const {
        if (rank == 0)
            std::cout << "acceptance cases: " << passed << " passed, " << failed << " failed\n";
        return failed ? 1 : 0;
    }
};
struct PetscSession {
    PetscSession(int& argc, char**& argv) {
        require(PetscInitialize(&argc, &argv, nullptr, nullptr) == PETSC_SUCCESS,
                "PetscInitialize");
    }
    ~PetscSession() { PetscFinalize(); }
};

inline Vec add(Vec a, Vec b) {
    for (int d = 0; d < 3; ++d)
        a[d] += b[d];
    return a;
}
inline Vec mul(Vec a, double s) {
    for (auto& x : a)
        x *= s;
    return a;
}
inline Vec sub(Vec a, Vec b) {
    return add(a, mul(b, -1));
}
inline double dot(Vec a, Vec b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
inline Vec cross(Vec a, Vec b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
inline double norm(Vec a) {
    return std::sqrt(dot(a, a));
}
inline std::vector<double> uniform(int n) {
    std::vector<double> axis(static_cast<std::size_t>(n) + 1);
    for (int i = 0; i <= n; ++i)
        axis[i] = double(i) / n;
    return axis;
}

// Complete, synthetic input. Coordinates determine geometry; dense Morton IDs
// deliberately disagree with row-major columns for nx,ny >= 3.
struct TestAxes {
    std::vector<double> x, y, z;
};
inline mesh::HexMeshArrays boxes(TestAxes axes, bool layered = false, int ranks = 1) {
    mesh::HexMeshArrays m;
    m.axes = {std::move(axes.x), std::move(axes.y), std::move(axes.z)};
    const auto nx = m.axes[0].size() - 1, ny = m.axes[1].size() - 1, nz = m.axes[2].size() - 1;
    std::vector<std::size_t> columns(nx * ny), nodes((nx + 1) * (ny + 1));
    std::iota(columns.begin(), columns.end(), 0);
    std::iota(nodes.begin(), nodes.end(), 0);
    auto sort = [](auto& ids, std::size_t width) {
        std::sort(ids.begin(), ids.end(), [=](auto a, auto b) {
            return mesh::morton(static_cast<std::uint32_t>(a % width),
                                static_cast<std::uint32_t>(a / width)) <
                   mesh::morton(static_cast<std::uint32_t>(b % width),
                                static_cast<std::uint32_t>(b / width));
        });
    };
    sort(columns, nx);
    sort(nodes, nx + 1);
    std::vector<mesh::Local> node_index(nodes.size());
    for (std::size_t col = 0; col < nodes.size(); ++col) {
        node_index[nodes[col]] = static_cast<mesh::Local>(col);
        for (std::size_t k = 0; k <= nz; ++k) {
            m.vertices.insert(
                m.vertices.end(),
                {m.axes[0][nodes[col] % (nx + 1)], m.axes[1][nodes[col] / (nx + 1)], m.axes[2][k]});
            m.node_gid.push_back(static_cast<std::int64_t>(m.node_gid.size()));
            m.node_column.push_back(static_cast<std::int64_t>(col));
            m.node_layer.push_back(layered ? static_cast<mesh::Local>(k ? k - 1 : 0) : 0);
        }
    }
    const auto layers = layered ? nz : 1;
    m.copper_layer_of_layer.assign(layers, -1);
    for (std::size_t l = 0; l < layers; ++l) {
        auto name = layered ? "L" + std::to_string(l) : "solid";
        m.attr_table.layers[name] = static_cast<mesh::Local>(l + 1);
        m.attr_table.entries.push_back({name, "", 1, 1});
        m.attr_table.references["layer:" + name] = {static_cast<mesh::Local>(l + 1)};
    }
    m.attr_table.materials["synthetic"] = 1;
    m.attr_table.bodies["solid"] = 1;
    auto node = [&](std::size_t i, std::size_t j, std::size_t k) {
        return static_cast<mesh::Local>(node_index[i + (nx + 1) * j] * (nz + 1) + k);
    };
    constexpr int face_nodes[6][4] = {
        {0, 4, 7, 3}, {1, 2, 6, 5}, {0, 1, 5, 4}, {3, 7, 6, 2}, {0, 3, 2, 1}, {4, 5, 6, 7}};
    for (std::size_t col = 0; col < columns.size(); ++col) {
        auto i = columns[col] % nx, j = columns[col] / nx;
        for (std::size_t k = 0; k < nz; ++k) {
            std::array<mesh::Local, 8> hex{node(i, j, k),
                                           node(i + 1, j, k),
                                           node(i + 1, j + 1, k),
                                           node(i, j + 1, k),
                                           node(i, j, k + 1),
                                           node(i + 1, j, k + 1),
                                           node(i + 1, j + 1, k + 1),
                                           node(i, j + 1, k + 1)};
            m.hex.insert(m.hex.end(), hex.begin(), hex.end());
            m.attribute.push_back(layered ? static_cast<mesh::Local>(k + 1) : 1);
            m.elem_gid.push_back(static_cast<std::int64_t>(col * nz + k));
            m.partitioning.push_back(static_cast<mesh::Local>(col % ranks));
            const bool boundary[6] = {
                i == 0, i + 1 == nx, j == 0, j + 1 == ny, k == 0, k + 1 == nz};
            for (int s = 0; s < 6; ++s)
                if (boundary[s]) {
                    for (int v : face_nodes[s])
                        m.bdr_quad.push_back(hex[v]);
                    m.bdr_attribute.push_back(s + 1);
                }
        }
    }
    for (int s = 1; s <= 6; ++s)
        m.face_sets["faceset:" + std::to_string(s)] = {s};
    return m;
}
inline mesh::HexMeshArrays cube(int n, int ranks = 1) {
    return boxes({uniform(n), uniform(n), uniform(n)}, false, ranks);
}
inline Vec vertex(const mesh::HexMeshArrays& m, std::size_t e, int local) {
    auto v = static_cast<std::size_t>(m.hex[8 * e + local]);
    return {m.vertices[3 * v], m.vertices[3 * v + 1], m.vertices[3 * v + 2]};
}
inline Poly box_poly(Vec lo, Vec hi) {
    std::array<Vec, 8> v{{lo,
                          {hi[0], lo[1], lo[2]},
                          {hi[0], hi[1], lo[2]},
                          {lo[0], hi[1], lo[2]},
                          {lo[0], lo[1], hi[2]},
                          {hi[0], lo[1], hi[2]},
                          hi,
                          {lo[0], hi[1], hi[2]}}};
    constexpr int ids[6][4] = {
        {0, 4, 7, 3}, {1, 2, 6, 5}, {0, 1, 5, 4}, {3, 7, 6, 2}, {0, 3, 2, 1}, {4, 5, 6, 7}};
    Poly p;
    for (const auto& face : ids) {
        Ring r;
        for (int id : face)
            r.push_back(v[id]);
        p.push_back(std::move(r));
    }
    return p;
}
struct SurfaceMoment {
    double area = 0, quadratic = 0;
    Vec first{}, area_vector{};
    Vec centroid() const { return mul(first, 1 / area); }
};
// Signed fan triangles work for concave rings, including fans outside a face.
inline SurfaceMoment surface(const Ring& r) {
    SurfaceMoment out;
    Vec normal{};
    for (std::size_t i = 1; i + 1 < r.size(); ++i)
        normal = add(normal, cross(sub(r[i], r[0]), sub(r[i + 1], r[0])));
    normal = mul(normal, 1 / norm(normal));
    for (std::size_t i = 1; i + 1 < r.size(); ++i) {
        auto a = r[0], b = r[i], c = r[i + 1];
        auto s = mul(cross(sub(b, a), sub(c, a)), .5);
        const double area = dot(s, normal);
        out.area += area;
        out.area_vector = add(out.area_vector, s);
        out.first = add(out.first, mul(add(a, add(b, c)), area / 3));
        out.quadratic +=
            area * (dot(a, a) + dot(b, b) + dot(c, c) + dot(a, b) + dot(a, c) + dot(b, c)) / 6;
    }
    return out;
}
struct VolumeMoment {
    double volume = 0, quadratic = 0;
    Vec first{};
    Vec centroid() const { return mul(first, 1 / volume); }
};
// Boundary tetrahedra use a nearby vertex and their SIGNED determinant. Wide
// accumulation avoids cancellation for tiny cut volumes far from the origin.
// No absolute tetra volumes, centroid sampling, or mesh quadrature in the oracle.
inline VolumeMoment volume(const Poly& p) {
    if (p.empty())
        return {};
    const auto origin = p.front().front();
    long double total = 0, quadratic = 0;
    std::array<long double, 3> first{};
    for (const auto& r : p)
        for (std::size_t i = 1; i + 1 < r.size(); ++i) {
            std::array<std::array<long double, 3>, 4> x{};
            const std::array<Vec, 4> points{origin, r[0], r[i], r[i + 1]};
            for (int j = 0; j < 4; ++j)
                for (int d = 0; d < 3; ++d)
                    x[j][d] = points[j][d];
            long double v = 0;
            for (int d = 0; d < 3; ++d) {
                const auto a = (d + 1) % 3, b = (d + 2) % 3;
                v += (x[1][d] - x[0][d]) *
                     ((x[2][a] - x[0][a]) * (x[3][b] - x[0][b]) -
                      (x[2][b] - x[0][b]) * (x[3][a] - x[0][a])) /
                     6;
            }
            total += v;
            for (int d = 0; d < 3; ++d)
                for (int j = 0; j < 4; ++j) {
                    first[d] += v * x[j][d] / 4;
                    quadratic += v * x[j][d] * x[j][d] / 10;
                    for (int k = j + 1; k < 4; ++k)
                        quadratic += v * x[j][d] * x[k][d] / 10;
                }
        }
    return {static_cast<double>(total),
            static_cast<double>(quadratic),
            {static_cast<double>(first[0]),
             static_cast<double>(first[1]),
             static_cast<double>(first[2])}};
}
inline Poly prism(const std::vector<std::array<double, 2>>& ring, double lo = 0, double hi = 1) {
    Ring bottom, top;
    for (auto p : ring) {
        bottom.push_back({p[0], p[1], lo});
        top.push_back({p[0], p[1], hi});
    }
    std::reverse(bottom.begin(), bottom.end());
    Poly out{bottom, top};
    for (std::size_t i = 0; i < top.size(); ++i) {
        const auto j = (i + 1) % top.size();
        out.push_back({{top[i][0], top[i][1], lo}, {top[j][0], top[j][1], lo}, top[j], top[i]});
    }
    return out;
}
inline Poly l_prism() {
    return prism({{0, 0}, {2, 0}, {2, 1}, {1, 1}, {1, 2}, {0, 2}});
}

// Independent half-space clipping is used ONLY to construct exact MMS moments.
inline Poly clip(Poly p, Vec n, double d) {
    bool outside = false, inside = false;
    for (const auto& ring : p)
        for (auto x : ring) {
            outside |= dot(n, x) > d;
            inside |= dot(n, x) < d;
        }
    if (!outside)
        return p;
    if (!inside)
        return {};
    Poly out;
    Ring cap;
    auto cap_vertex = [&](Vec x) {
        if (std::none_of(cap.begin(), cap.end(), [&](auto y) { return norm(sub(x, y)) < 1e-13; }))
            cap.push_back(x);
    };
    for (const auto& r : p) {
        Ring next;
        for (std::size_t j = 0; j < r.size(); ++j) {
            auto a = r[j], b = r[(j + 1) % r.size()];
            const double fa = dot(n, a) - d, fb = dot(n, b) - d;
            if (fa <= 0)
                next.push_back(a);
            if (std::abs(fa) <=
                8 * std::numeric_limits<double>::epsilon() * std::max(1., std::abs(d)))
                cap_vertex(a);
            if ((fa < 0 && fb > 0) || (fa > 0 && fb < 0)) {
                auto x = add(a, mul(sub(b, a), fa / (fa - fb)));
                next.push_back(x);
                cap_vertex(x);
            }
        }
        if (next.size() >= 3)
            out.push_back(std::move(next));
    }
    if (cap.size() >= 3) {
        Vec center{};
        for (auto v : cap)
            center = add(center, mul(v, 1. / cap.size()));
        auto u = cross(n, std::abs(n[0]) < .9 * norm(n) ? Vec{1, 0, 0} : Vec{0, 1, 0});
        u = mul(u, 1 / norm(u));
        auto v = cross(mul(n, 1 / norm(n)), u);
        std::sort(cap.begin(), cap.end(), [&](auto a, auto b) {
            auto aa = sub(a, center), bb = sub(b, center);
            return std::atan2(dot(aa, v), dot(aa, u)) < std::atan2(dot(bb, v), dot(bb, u));
        });
        out.push_back(std::move(cap));
    }
    return out;
}
inline Poly element_poly(const mesh::HexMeshArrays& m,
                         std::size_t e,
                         const mesh::cut::CutGeometry& geometry = {}) {
    auto p = box_poly(vertex(m, e, 0), vertex(m, e, 6));
    for (const auto& plane : geometry.planes)
        p = clip(std::move(p), plane.n, plane.d);
    return p;
}
inline fv::FvGrid hand_grid(const mesh::HexMeshArrays& m, const std::vector<Poly>& cells) {
    fv::FvGrid g;
    g.axes = m.axes;
    g.nz = m.axes[2].size() - 1;
    for (std::size_t c = 0; c < cells.size(); ++c) {
        auto v = volume(cells[c]);
        g.cv_elem.push_back(0);
        g.cv_elem_gid.push_back(m.elem_gid[0]);
        g.cv_attribute.push_back(1);
        g.cv_layer.push_back(1);
        g.cv_rank.push_back(0);
        g.cv_kind.push_back(1);
        g.cv_volume.push_back(v.volume);
        g.cv_volfrac.push_back(v.volume / volume(element_poly(m, 0)).volume);
        g.cv_copper_fraction.push_back(0);
        g.cv_centroid.push_back(v.centroid());
        for (const auto& ring : cells[c]) {
            auto s = surface(ring);
            fv::FvFace f;
            f.lo = static_cast<std::int64_t>(c);
            f.side = 6;
            f.area = s.area;
            f.area_vector = s.area_vector;
            f.centroid = s.centroid();
            g.cv_faces.push_back(static_cast<std::int64_t>(g.faces.size()));
            g.faces.push_back(f);
        }
        g.cv_face_off.push_back(static_cast<std::int64_t>(g.cv_faces.size()));
    }
    return g;
}
inline void geometry_gate(const fv::FvGrid& g) {
    require(!g.cv_volume.empty(), "grid must retain physical CVs");
    require(g.cv_face_off.size() == g.cv_volume.size() + 1, "CSR offsets");
    require(g.cv_face_off.front() == 0 &&
                g.cv_face_off.back() == static_cast<std::int64_t>(g.cv_faces.size()),
            "CSR endpoints");
    for (std::size_t c = 0; c < g.cv_volume.size(); ++c) {
        require(std::isfinite(g.cv_volume[c]) && g.cv_volume[c] > 0, "positive finite CV volume");
        double nr[3][3]{};
        Vec sum{};
        for (auto j = g.cv_face_off[c]; j < g.cv_face_off[c + 1]; ++j) {
            const auto& f = g.faces.at(static_cast<std::size_t>(g.cv_faces[j]));
            require(f.lo == static_cast<std::int64_t>(c) || f.hi == static_cast<std::int64_t>(c),
                    "CSR incidence");
            require(std::isfinite(f.area) && f.area > 0, "usable face area");
            const auto s = mul(f.area_vector, f.lo == static_cast<std::int64_t>(c) ? 1 : -1);
            sum = add(sum, s);
            auto r = sub(f.centroid, g.cv_centroid[c]);
            for (int a = 0; a < 3; ++a)
                for (int b = 0; b < 3; ++b)
                    nr[a][b] += s[a] * r[b];
        }
        for (int a = 0; a < 3; ++a) {
            near(sum[a], 0, 1e-12 * std::pow(g.cv_volume[c], 2. / 3), 0, "closed surface");
            for (int b = 0; b < 3; ++b)
                near(nr[a][b],
                     a == b ? g.cv_volume[c] : 0,
                     1e-10 * g.cv_volume[c],
                     0,
                     "N^T R = V I");
        }
    }
    require(std::isfinite(g.max_identity_defect) && g.max_identity_defect <= 1e-10,
            "reported geometry defect");
}

inline fv::ThermalBc dirichlet(std::string target, double T) {
    fv::ThermalBc b{};
    b.kind = fv::ThermalBc::Kind::Dirichlet;
    b.target = std::move(target);
    b.T = T;
    return b;
}
inline fv::ThermalProblem
problem(const fv::FvGrid& g, Vec k = {1, 2, 3}, const std::string& pc = "jacobi") {
    fv::ThermalProblem p;
    p.k.assign(g.cv_volume.size(), k);
    p.q_vol.assign(g.cv_volume.size(), 0);
    p.preconditioner = pc;
    p.rtol = 1e-14;
    p.max_iter = 10000;
    return p;
}
inline std::string target(const fv::FvFace& f) {
    static const std::array<std::string, 6> names{
        "face:xmin", "face:xmax", "face:ymin", "face:ymax", "face:zmin", "face:zmax"};
    return f.side == 6 ? "boundary:embedded" : names.at(static_cast<std::size_t>(f.side));
}
struct Polynomial {
    double offset = 0;
    Vec linear{};
    bool quadratic = false;
    double cell(const Poly& p) const {
        auto v = volume(p);
        return offset + dot(linear, v.centroid()) + (quadratic ? v.quadratic / v.volume : 0);
    }
    double face(const Ring& p) const {
        auto s = surface(p);
        return offset + dot(linear, s.centroid()) + (quadratic ? s.quadratic / s.area : 0);
    }
    Vec gradient(Vec x) const { return add(linear, mul(x, quadratic ? 2 : 0)); }
};
inline const Ring& matching_face(const Poly& p, const fv::FvFace& f) {
    for (const auto& ring : p) {
        auto s = surface(ring);
        if (norm(sub(s.centroid(), f.centroid)) <= 1e-11 &&
            std::abs(s.area - f.area) <= 1e-11 * f.area)
            return ring;
    }
    throw std::runtime_error("no independent polygon for solver face");
}
inline double exact_flux(const fv::FvFace& f, const Polynomial& u, Vec k) {
    auto grad = u.gradient(f.centroid);
    for (int a = 0; a < 3; ++a)
        grad[a] *= k[a];
    return -dot(grad, f.area_vector);
}
inline fv::ThermalProblem manufactured(const fv::FvGrid& g,
                                       const std::vector<Poly>& polys,
                                       Polynomial u,
                                       const std::string& pc = "jacobi",
                                       bool robin_all = false) {
    auto p = problem(g, {1, 2, 3}, pc);
    if (u.quadratic)
        std::fill(p.q_vol.begin(), p.q_vol.end(), -12);
    for (std::size_t i = 0; i < g.faces.size(); ++i) {
        const auto& f = g.faces[i];
        if (f.hi >= 0)
            continue;
        const auto& ring = matching_face(polys.at(static_cast<std::size_t>(f.lo)), f);
        auto b = dirichlet(target(f), u.face(ring));
        b.face = i; // Optional explicit face selector needed for spatial MMS data.
        if (f.side == 6 || robin_all) {
            b.kind = fv::ThermalBc::Kind::Robin;
            b.h = 5;
            b.T_inf = u.face(ring) - exact_flux(f, u, p.k[f.lo]) / (b.h * f.area);
        }
        p.bcs.push_back(b);
    }
    return p;
}
inline std::vector<Poly> polynomials(const fv::FvGrid& g,
                                     const mesh::HexMeshArrays& m,
                                     const mesh::cut::CutGeometry& geometry = {}) {
    std::vector<Poly> out;
    for (auto e : g.cv_elem)
        out.push_back(element_poly(m, static_cast<std::size_t>(e), geometry));
    return out;
}
inline fv::ThermalResult solve(const fv::FvGrid& g,
                               const mesh::HexMeshArrays& m,
                               const fv::ThermalProblem& p,
                               MPI_Comm comm = MPI_COMM_WORLD) {
    auto r =
        take(comm == MPI_COMM_WORLD ? fv::solve_thermal(g, m, p, chipcae::Communicator::world())
                                    : fv::detail::solve_thermal_on_comm(g, m, p, comm),
             "solve_thermal must solve acceptance fixture");
    require(r.T_cell.size() == g.cv_volume.size(), "T_cell size");
    require(r.T_face.size() == g.faces.size() && r.face_flux.size() == g.faces.size(),
            "face field sizes");
    require(r.T_vertex.size() == m.vertices.size() / 3, "vertex field size");
    require(r.k_used.size() == p.k.size(), "k_used size");
    require(r.iterations >= 0 && std::isfinite(r.residual), "solver diagnostics");
    require(std::isfinite(r.max_local_condition) && r.max_local_condition >= 1,
            "local condition number");
    require(std::find(g.cv_elem_gid.begin(), g.cv_elem_gid.end(), r.max_condition_gid) !=
                g.cv_elem_gid.end(),
            "condition gid");
    for (std::size_t c = 0; c < p.k.size(); ++c)
        for (int d = 0; d < 3; ++d)
            near(r.k_used[c][d], p.k[c][d], 0, 1e-14, "k_used copies supplied diagonal");
    return r;
}
inline void conservation(const fv::ThermalResult& r) {
    if (r.cell_balance_max > 1e-12 || r.face_jump_max > 1e-9 || r.imbalance > 1e-9)
        std::cout << std::setprecision(17)
                  << "measured failed conservation: cell=" << r.cell_balance_max
                  << " face=" << r.face_jump_max << " imbalance=" << r.imbalance << '\n';
    require(std::isfinite(r.cell_balance_max) && r.cell_balance_max >= 0 &&
                r.cell_balance_max <= 1e-12,
            "cell balance <= 1e-12");
    require(std::isfinite(r.face_jump_max) && r.face_jump_max >= 0 && r.face_jump_max <= 1e-9,
            "face jump <= 1e-9");
    require(std::isfinite(r.imbalance) && r.imbalance >= 0 && r.imbalance <= 1e-9,
            "energy imbalance <= 1e-9");
}
inline void affine_check(const fv::FvGrid& g,
                         const mesh::HexMeshArrays& m,
                         const std::vector<Poly>& polys,
                         Polynomial u,
                         const std::string& pc,
                         double tolerance,
                         const std::string& stabilization = "diag") {
    auto p = manufactured(g, polys, u, pc);
    p.stabilization = stabilization;
    auto r = solve(g, m, p);
    double scale = 1;
    for (const auto& poly : polys)
        scale = std::max(scale, std::abs(u.cell(poly)));
    for (std::size_t c = 0; c < polys.size(); ++c)
        near(r.T_cell[c], u.cell(polys[c]), tolerance * scale, 0, "affine cell average");
    double flux_scale = 0;
    for (const auto& f : g.faces)
        flux_scale = std::max(flux_scale, std::abs(exact_flux(f, u, p.k[f.lo])));
    for (std::size_t f = 0; f < g.faces.size(); ++f) {
        auto exact = exact_flux(g.faces[f], u, p.k[g.faces[f].lo]);
        near(r.face_flux[f],
             exact,
             exact == 0 ? tolerance * std::max(1., flux_scale) : 0,
             exact == 0 ? 0 : 1e-11,
             "affine integrated face flux (outward from lo)");
    }
}

// Independent cell-centred TPFA oracle, including distance-weighted harmonic
// resistance. This dense Gaussian elimination never calls MFD or PETSc.
inline std::vector<double> dense_solve(std::vector<std::vector<double>> a, std::vector<double> b) {
    const auto n = b.size();
    for (std::size_t j = 0; j < n; ++j) {
        auto pivot = j;
        for (std::size_t i = j + 1; i < n; ++i)
            if (std::abs(a[i][j]) > std::abs(a[pivot][j]))
                pivot = i;
        require(std::abs(a[pivot][j]) > 1e-300, "independent TPFA nonsingular");
        std::swap(a[pivot], a[j]);
        std::swap(b[pivot], b[j]);
        for (std::size_t i = j + 1; i < n; ++i) {
            double f = a[i][j] / a[j][j];
            for (std::size_t k = j; k < n; ++k)
                a[i][k] -= f * a[j][k];
            b[i] -= f * b[j];
        }
    }
    for (std::size_t i = n; i-- > 0;) {
        for (std::size_t j = i + 1; j < n; ++j)
            b[i] -= a[i][j] * b[j];
        b[i] /= a[i][i];
    }
    return b;
}
inline std::vector<double> tpfa(const mesh::HexMeshArrays& m, const fv::ThermalProblem& p) {
    auto n = m.attribute.size();
    std::vector<std::vector<double>> a(n, std::vector<double>(n));
    std::vector<double> b(n);
    std::vector<Vec> lo(n), hi(n), center(n);
    for (std::size_t c = 0; c < n; ++c) {
        lo[c] = vertex(m, c, 0);
        hi[c] = vertex(m, c, 6);
        center[c] = mul(add(lo[c], hi[c]), .5);
        auto h = sub(hi[c], lo[c]);
        b[c] = p.q_vol[c] * h[0] * h[1] * h[2];
    }
    static const std::array<std::string, 6> targets{
        "face:xmin", "face:xmax", "face:ymin", "face:ymax", "face:zmin", "face:zmax"};
    for (std::size_t c = 0; c < n; ++c)
        for (int side = 0; side < 6; ++side) {
            auto d = side / 2;
            const double coord = side % 2 ? hi[c][d] : lo[c][d];
            auto h = sub(hi[c], lo[c]);
            double area = h[(d + 1) % 3] * h[(d + 2) % 3];
            bool neighbor = false;
            for (std::size_t j = 0; j < n; ++j) {
                if (j == c)
                    continue;
                bool adjacent = std::abs(coord - (side % 2 ? lo[j][d] : hi[j][d])) < 1e-14;
                for (int t = 0; t < 3; ++t)
                    if (t != d)
                        adjacent &= std::abs(lo[c][t] - lo[j][t]) < 1e-14 &&
                                    std::abs(hi[c][t] - hi[j][t]) < 1e-14;
                if (!adjacent)
                    continue;
                neighbor = true;
                double t = area / (std::abs(coord - center[c][d]) / p.k[c][d] +
                                   std::abs(center[j][d] - coord) / p.k[j][d]);
                a[c][c] += t;
                a[c][j] -= t;
            }
            if (neighbor)
                continue;
            for (const auto& bc : p.bcs) {
                if (bc.target != targets[side])
                    continue;
                require(!bc.face, "TPFA fixture uses target-wide BCs");
                double resistance = std::abs(coord - center[c][d]) / (p.k[c][d] * area);
                if (bc.kind == fv::ThermalBc::Kind::Flux) {
                    b[c] += bc.q * area;
                } else {
                    double t =
                        1 / (resistance +
                             (bc.kind == fv::ThermalBc::Kind::Robin ? 1 / (bc.h * area) : 0));
                    a[c][c] += t;
                    b[c] += t * (bc.kind == fv::ThermalBc::Kind::Robin ? bc.T_inf : bc.T);
                }
            }
        }
    return dense_solve(std::move(a), std::move(b));
}

struct Errors {
    double h = 0, l2 = 0, linf = 0, flux = 0;
};
inline Errors quadratic_errors(const fv::FvGrid& g,
                               const mesh::HexMeshArrays& m,
                               const std::vector<Poly>& polys,
                               const std::string& pc,
                               const std::string& stabilization) {
    Polynomial u{0, {}, true};
    auto p = manufactured(g, polys, u, pc);
    p.stabilization = stabilization;
    auto r = solve(g, m, p);
    Errors e;
    double v = 0, area = 0;
    for (std::size_t c = 0; c < polys.size(); ++c) {
        auto vm = volume(polys[c]);
        const double error = std::abs(r.T_cell[c] - u.cell(polys[c]));
        e.l2 += vm.volume * error * error;
        v += vm.volume;
        e.linf = std::max(e.linf, error);
        for (const auto& fa : polys[c])
            for (auto a : fa)
                for (const auto& fb : polys[c])
                    for (auto b : fb)
                        e.h = std::max(e.h, norm(sub(a, b)));
    }
    for (std::size_t i = 0; i < g.faces.size(); ++i) {
        const auto& f = g.faces[i];
        const auto& ring = matching_face(polys[f.lo], f);
        auto s = surface(ring);
        auto gradient = mul(s.centroid(), 2);
        for (int d = 0; d < 3; ++d)
            gradient[d] *= p.k[f.lo][d];
        double density_error = (r.face_flux[i] + dot(gradient, f.area_vector)) / s.area;
        e.flux += s.area * density_error * density_error;
        area += s.area;
    }
    e.l2 = std::sqrt(e.l2 / v);
    e.flux = std::sqrt(e.flux / area);
    return e;
}
inline double order(double coarse, double fine, double hc, double hf) {
    require(coarse > 0 && fine > 0 && hc > hf, "positive errors and decreasing measured diameter");
    return std::log(coarse / fine) / std::log(hc / hf);
}
inline void
convergence(const std::vector<Errors>& e, bool cut, const std::string& label, bool gate = true) {
    for (std::size_t i = 1; i < e.size(); ++i) {
        auto cell = order(e[i - 1].l2, e[i].l2, e[i - 1].h, e[i].h);
        auto flux = order(e[i - 1].flux, e[i].flux, e[i - 1].h, e[i].h);
        auto inf = order(e[i - 1].linf, e[i].linf, e[i - 1].h, e[i].h);
        std::cout << std::setprecision(12) << "measured " << label << " refinement=" << i
                  << " h=" << e[i].h << " L2=" << e[i].l2 << " Linf=" << e[i].linf
                  << " flux_density_L2=" << e[i].flux << " orders=" << cell << ',' << inf << ','
                  << flux << '\n';
        if (gate) {
            require(cell >= (cut ? 1.8 : 1.9), label + ": cell L2 order");
            require(cut ? inf >= .9 : flux >= .75, label + ": Linf/flux order");
        }
    }
}
inline double relative_difference(const std::vector<double>& a, const std::vector<double>& b) {
    require(a.size() == b.size(), "comparable field lengths");
    double error = 0, scale = 1e-300;
    for (std::size_t i = 0; i < a.size(); ++i) {
        require(std::isfinite(a[i]) && std::isfinite(b[i]), "finite fields");
        error = std::max(error, std::abs(a[i] - b[i]));
        scale = std::max(scale, std::abs(b[i]));
    }
    return error / scale;
}
inline bool bitwise(const std::vector<double>& a, const std::vector<double>& b) {
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::bit_cast<std::uint64_t>(a[i]) != std::bit_cast<std::uint64_t>(b[i]))
            return false;
    return true;
}
inline std::vector<double> all_fields(const fv::ThermalResult& r) {
    auto out = r.T_cell;
    for (const auto* f : {&r.T_face, &r.face_flux, &r.T_vertex})
        out.insert(out.end(), f->begin(), f->end());
    for (auto k : r.k_used)
        out.insert(out.end(), k.begin(), k.end());
    out.insert(out.end(),
               {double(r.iterations),
                r.residual,
                r.heat_in,
                r.heat_out,
                r.imbalance,
                r.cell_balance_max,
                r.face_jump_max,
                r.max_local_condition});
    return out;
}
inline void rank_identical(const fv::ThermalResult& r) {
    auto local = all_fields(r), root = local;
    MPI_Bcast(root.data(), static_cast<int>(root.size()), MPI_DOUBLE, 0, MPI_COMM_WORLD);
    require(bitwise(local, root), "all result fields and diagnostics agree across ranks");
    std::array<std::int64_t, 2> ids{r.max_condition_gid, r.gamma_floor_count}, root_ids = ids;
    MPI_Bcast(root_ids.data(), 2, MPI_INT64_T, 0, MPI_COMM_WORLD);
    require(ids == root_ids, "integer diagnostics agree across ranks");
}

inline mesh::cut::CutGeometry oblique(bool chamfer = false) {
    mesh::cut::CutGeometry geometry;
    geometry.planes = {{0, {1, .37, .21}, .953}};
    if (chamfer) {
        geometry.planes.push_back({0, {-.2, 1, .6}, 1.071});
        geometry.planes.push_back({0, {.3, -.4, 1}, .887});
    }
    return geometry;
}
struct CutFixture {
    mesh::HexMeshArrays mesh;
    mesh::cut::CutLayerArrays cut;
    mesh::cut::CutGeometry geometry;
};
inline CutFixture cut_fixture(int n, mesh::cut::CutGeometry geometry = oblique(), int ranks = 1) {
    CutFixture f;
    f.mesh = cube(n, ranks);
    f.geometry = std::move(geometry);
    mesh::cut::CutOptions options;
    options.quad_degree = 2;  // Exact degree two, without large stress fixtures.
    options.small_sub_vf = 0; // Every unlisted positive-volume sliver must survive.
    f.cut = take(mesh::cut::build_cut_layer(std::as_const(f.mesh), f.geometry, options),
                 "synthetic plane-cut fixture");
    return f;
}

// Minimal exact quadrature for an axis-aligned sub-box. This synthetic producer
// allows an explicit small_subs list without relying on a producer threshold.
inline void append_sub_box(
    mesh::cut::CutLayerArrays& cut, const mesh::HexMeshArrays& m, std::size_t e, Vec lo, Vec hi) {
    auto parent_lo = vertex(m, e, 0), parent_hi = vertex(m, e, 6);
    auto size = sub(parent_hi, parent_lo);
    double jac = size[0] * size[1] * size[2];
    auto p = box_poly(lo, hi);
    auto v = volume(p);
    auto xi = [&](Vec x) {
        for (int d = 0; d < 3; ++d)
            x[d] = (x[d] - parent_lo[d]) / size[d];
        return x;
    };
    cut.cut_elem.push_back(static_cast<mesh::Local>(e));
    cut.cut_elem_gid.push_back(m.elem_gid[e]);
    cut.sub_attribute.push_back(m.attribute[e]);
    cut.sub_volfrac.push_back(v.volume / jac);
    cut.sub_off.push_back(static_cast<mesh::Local>(cut.sub_attribute.size()));
    auto centroid = xi(v.centroid());
    for (int d = 0; d < 3; ++d)
        cut.vq_xi[d].push_back(centroid[d]);
    cut.vq_w.push_back(v.volume / jac);
    cut.vq_off.push_back(static_cast<mesh::Local>(cut.vq_w.size()));
    cut.bq_off.push_back(static_cast<mesh::Local>(cut.bq_w.size()));
    Vec exterior{};
    for (int side = 0; side < 6; ++side) {
        auto s = surface(p[side]);
        auto center = xi(s.centroid());
        const int d = side / 2;
        const bool axial = std::abs(center[d] - (side % 2)) < 1e-14;
        cut.sq_side.push_back(axial ? side : -1);
        cut.sq_w.push_back(s.area);
        for (int a = 0; a < 3; ++a)
            cut.sq_xi[a].push_back(center[a]);
        if (axial) {
            exterior = add(exterior, s.area_vector);
        } else {
            for (std::size_t j = 1; j + 1 < p[side].size(); ++j)
                for (auto x : {p[side][0], p[side][j], p[side][j + 1]}) {
                    auto ref = xi(x), normal = mul(s.area_vector, 1 / s.area);
                    cut.eb_points.insert(cut.eb_points.end(), x.begin(), x.end());
                    cut.eb_xi.insert(cut.eb_xi.end(), ref.begin(), ref.end());
                    cut.eb_normals.insert(cut.eb_normals.end(), normal.begin(), normal.end());
                    cut.eb_parent_elem.push_back(static_cast<mesh::Local>(e));
                }
        }
    }
    cut.sq_off.push_back(static_cast<mesh::Local>(cut.sq_w.size()));
    cut.exterior_area_vector.push_back(exterior);
}
} // namespace fvm_test
