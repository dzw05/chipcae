#pragma once
#include "chipcae/material/copper_laminate.hpp"
#include "chipcae/solvers/fvm/elastic.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <numeric>
#include <petscsys.h>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace elastic_test {
using namespace chipcae::solvers::fvm;
using V = std::array<double, 3>;
using M3 = std::array<double, 9>;
using M6 = std::array<double, 36>;
inline V add(V a, const V& b) {
    for (int i = 0; i < 3; ++i)
        a[i] += b[i];
    return a;
}
inline V scale(V a, double s) {
    for (double& v : a)
        v *= s;
    return a;
}
inline V sub(V a, const V& b) {
    return add(a, scale(b, -1));
}
inline double dot(const V& a, const V& b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
inline V cross(const V& a, const V& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
inline double norm(const V& a) {
    return std::sqrt(dot(a, a));
}
inline V mul(const M3& a, const V& x) {
    V y{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            y[i] += a[3 * i + j] * x[j];
    return y;
}
inline void require(bool ok, const std::string& why) {
    if (!ok)
        throw std::runtime_error(why);
}
inline void near(double got, double want, double atol, double rtol, const std::string& why) {
    require(std::isfinite(got) && std::abs(got - want) <= atol + rtol * std::abs(want),
            why + ": got=" + std::to_string(got) + " want=" + std::to_string(want));
}
inline double max_norm(const std::vector<V>& u) {
    double m = 0;
    for (const auto& v : u)
        m = std::max(m, norm(v));
    return m;
}
inline void field_near(const std::vector<V>& got,
                       const std::vector<V>& want,
                       double atol,
                       double rtol,
                       const std::string& why) {
    require(got.size() == want.size(), why + ": array length");
    double error = 0;
    for (std::size_t i = 0; i < got.size(); ++i) {
        require(
            std::all_of(got[i].begin(), got[i].end(), [](double v) { return std::isfinite(v); }),
            why + ": nonfinite displacement");
        error = std::max(error, norm(sub(got[i], want[i])));
    }
    const double limit = atol + rtol * max_norm(want);
    require(error <= limit,
            why + ": max error=" + std::to_string(error) + " limit=" + std::to_string(limit));
}
inline std::vector<double> axis(int n, double length = 1, double ratio = 1) {
    std::vector<double> x(static_cast<std::size_t>(n) + 1);
    double sum = 0;
    for (int i = 0; i < n; ++i)
        sum += std::pow(ratio, i);
    for (int i = 0; i < n; ++i)
        x[i + 1] = x[i] + length * std::pow(ratio, i) / sum;
    x.back() = length;
    return x;
}
inline ElasticMesh grid(const std::vector<double>& x,
                        const std::vector<double>& y,
                        const std::vector<double>& z,
                        int ranks = 1) {
    const std::array<int, 3> n{static_cast<int>(x.size()) - 1,
                               static_cast<int>(y.size()) - 1,
                               static_cast<int>(z.size()) - 1};
    const std::array<std::vector<double>, 3> axes{x, y, z};
    auto index = [&](const std::array<int, 3>& ijk) {
        return static_cast<std::int64_t>((ijk[0] * n[1] + ijk[1]) * n[2] + ijk[2]);
    };
    ElasticMesh m;
    m.z_top = z.back();
    for (int i = 0; i < n[0]; ++i)
        for (int j = 0; j < n[1]; ++j)
            for (int k = 0; k < n[2]; ++k) {
                const std::array<int, 3> ijk{i, j, k};
                const auto c = index(ijk);
                m.centroid.push_back(
                    {(x[i] + x[i + 1]) / 2, (y[j] + y[j + 1]) / 2, (z[k] + z[k + 1]) / 2});
                m.volume.push_back((x[i + 1] - x[i]) * (y[j + 1] - y[j]) * (z[k + 1] - z[k]));
                m.gid.push_back(1000003 - 17 * c); // Deliberately unrelated to input order.
                m.layer.push_back(k);
                m.rank.push_back((i * n[1] + j) % ranks); // A column stays on one rank.
                for (int d = 0; d < 3; ++d) {
                    for (int sign : {-1, 1}) {
                        if (sign < 0 && ijk[d] > 0)
                            continue;
                        auto next = ijk;
                        next[d] += sign;
                        const bool internal = next[d] >= 0 && next[d] < n[d];
                        ElasticFace f;
                        f.lo = c;
                        f.hi = internal ? index(next) : -1;
                        f.side = internal ? -1 : 2 * d + (sign > 0 ? 1 : 0);
                        f.centroid = m.centroid.back();
                        f.centroid[d] = axes[d][ijk[d] + (sign > 0 ? 1 : 0)];
                        f.normal[d] = sign;
                        f.area = 1;
                        for (int q = 0; q < 3; ++q)
                            if (q != d)
                                f.area *= axes[q][ijk[q] + 1] - axes[q][ijk[q]];
                        m.faces.push_back(f);
                        m.is_embedded.push_back(0);
                    }
                }
            }
    return m;
}
inline ElasticMesh grid(int nx, int ny, int nz, int ranks = 1) {
    return grid(axis(nx), axis(ny), axis(nz), ranks);
}
inline V center(const ElasticMesh& m) {
    V c{};
    for (const auto& p : m.centroid)
        c = add(c, p);
    return scale(c, 1.0 / static_cast<double>(m.centroid.size()));
}
inline double length(const ElasticMesh& m) {
    V lo = m.centroid.front(), hi = lo;
    for (const auto& f : m.faces)
        for (int d = 0; d < 3; ++d) {
            lo[d] = std::min(lo[d], f.centroid[d]);
            hi[d] = std::max(hi[d], f.centroid[d]);
        }
    const auto extent = sub(hi, lo);
    return *std::max_element(extent.begin(), extent.end());
}
// Test-only dense elimination. The constitutive reference never calls solver algebra.
inline std::vector<double> linear_solve(std::vector<double> a, std::vector<double> b) {
    const auto n = b.size();
    require(a.size() == n * n, "reference matrix shape");
    for (std::size_t k = 0; k < n; ++k) {
        std::size_t p = k;
        for (std::size_t i = k + 1; i < n; ++i)
            if (std::abs(a[i * n + k]) > std::abs(a[p * n + k]))
                p = i;
        require(std::abs(a[p * n + k]) > 1e-30, "singular reference matrix");
        for (std::size_t j = k; j < n; ++j)
            std::swap(a[k * n + j], a[p * n + j]);
        std::swap(b[k], b[p]);
        const double pivot = a[k * n + k];
        for (std::size_t j = k; j < n; ++j)
            a[k * n + j] /= pivot;
        b[k] /= pivot;
        for (std::size_t i = 0; i < n; ++i) {
            if (i == k)
                continue;
            const double v = a[i * n + k];
            for (std::size_t j = k; j < n; ++j)
                a[i * n + j] -= v * a[k * n + j];
            b[i] -= v * b[k];
        }
    }
    return b;
}
inline M6 inverse(const M6& s) {
    M6 c{};
    for (int j = 0; j < 6; ++j) {
        std::vector<double> e(6);
        e[j] = 1;
        const auto col = linear_solve({s.begin(), s.end()}, e);
        for (int i = 0; i < 6; ++i)
            c[6 * i + j] = col[i];
    }
    return c;
}
struct Material {
    M6 C;
    V alpha;
};
inline Material isotropic(double E = 25e9, double nu = 0.3, double alpha = 1e-5) {
    M6 c{};
    const double mu = E / (2 * (1 + nu));
    const double lambda = E * nu / ((1 + nu) * (1 - 2 * nu));
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            c[6 * i + j] = lambda + (i == j ? 2 * mu : 0);
    for (int i = 3; i < 6; ++i)
        c[6 * i + i] = mu;
    return {c, {alpha, alpha, alpha}};
}
inline Material laminate(double f) {
    double p[9]{}, a[3]{};
    chipcae::material::copper_laminate(25e9, 0.3, 110e9, 0.34, 1e-5, 17e-6, f, p, a);
    M6 s{};
    for (int i = 0; i < 3; ++i)
        s[6 * i + i] = 1 / p[i];
    s[1] = s[6] = -p[3] / p[0];
    s[2] = s[12] = -p[4] / p[0];
    s[8] = s[13] = -p[5] / p[1];
    s[21] = 1 / p[8];
    s[28] = 1 / p[7];
    s[35] = 1 / p[6];
    return {inverse(s), {a[0], a[1], a[2]}};
}
inline Material orthotropic() {
    M6 s{};
    s[0] = 1 / 41e9;
    s[7] = 1 / 27e9;
    s[14] = 1 / 13e9;
    s[1] = s[6] = -0.19 / 41e9;
    s[2] = s[12] = -0.13 / 41e9;
    s[8] = s[13] = -0.17 / 27e9;
    s[21] = 1 / 3e9;
    s[28] = 1 / 5e9;
    s[35] = 1 / 7e9;
    return {inverse(s), {7e-6, 11e-6, 19e-6}};
}
inline ElasticProblem problem(const ElasticMesh& m, Material mat = isotropic(), double dT = 80) {
    ElasticProblem p;
    p.C.assign(m.volume.size(), mat.C);
    p.alpha.assign(m.volume.size(), mat.alpha);
    p.dT.assign(m.volume.size(), dT);
    p.rtol = 1e-13;
    p.max_iter = 10000;
    return p;
}
inline void prescribe(ElasticProblem& p, const ElasticMesh& m, const M3& a, const V& b = {}) {
    p.support = ElasticProblem::Support::Fixed;
    for (std::size_t f = 0; f < m.faces.size(); ++f)
        if (m.faces[f].hi < 0 && !m.is_embedded[f])
            p.prescribed.push_back(
                {static_cast<std::int64_t>(f), add(mul(a, m.faces[f].centroid), b)});
}
inline ElasticResult solve(const ElasticMesh& m, const ElasticProblem& p, MPI_Comm comm) {
    auto r = solve_elastic(m, p, comm);
    int local = r ? 1 : 0, all = 0;
    MPI_Allreduce(&local, &all, 1, MPI_INT, MPI_MIN, comm);
    require(all != 0,
            r ? "another rank rejected valid elastic problem"
              : "solve_elastic must succeed: " + r.error().reason);
    return std::move(r.value());
}
inline void check_output(const ElasticMesh& m, const ElasticProblem& p, const ElasticResult& r) {
    require(r.U_cell.size() == m.volume.size(), "U_cell preserves input count/order");
    for (const auto& u : r.U_cell)
        for (double v : u)
            require(std::isfinite(v), "finite cell displacement");
    require(r.U_face.size() == m.faces.size(), "U_face preserves face count/order");
    require(r.face_prescribed.size() == m.faces.size(), "face_prescribed length");
    require(std::isfinite(r.residual) && r.residual >= 0, "finite nonnegative residual");
    require(r.iterations >= 0 && r.iterations <= p.max_iter, "iteration bounds");
    require(r.warpage_method == "FVM top-face centroids; least-squares plane removal",
            "warpage method");
    require(r.U_mag_method == "FVM cell centers", "U magnitude method");
    near(r.U_mag_max, max_norm(r.U_cell), 1e-30, 1e-13, "cell magnitude maximum");
    require(std::isfinite(r.warpage_pv) && r.warpage_pv >= 0, "finite plane removal");
    std::vector<V> samples;
    for (std::size_t i = 0; i < m.faces.size(); ++i) {
        const auto& f = m.faces[i];
        if (f.hi < 0 && (f.side == 5 || (m.is_embedded[i] && f.normal[2] > 0.99 &&
                                         f.centroid[2] >= m.z_top - 1e-13)))
            samples.push_back({f.centroid[0], f.centroid[1], r.U_face[i][2]});
        bool expected = false;
        if (p.support == ElasticProblem::Support::Fixed && f.hi < 0 && !m.is_embedded[i]) {
            expected =
                std::find(p.fixed_side.begin(), p.fixed_side.end(), f.side) != p.fixed_side.end() ||
                std::find(p.fixed_layer.begin(), p.fixed_layer.end(), m.layer[f.lo]) !=
                    p.fixed_layer.end();
        }
        for (const auto& bc : p.prescribed)
            if (bc.face == static_cast<std::int64_t>(i)) {
                expected = true;
                // Required exact prescribed output (including signed/zero components).
                require(r.U_face[i] == bc.U, "prescribed face value returned exactly");
            }
        require((r.face_prescribed[i] != 0) == expected,
                "prescribed flag for face " + std::to_string(i));
        for (double v : r.U_face[i])
            require(std::isfinite(v), "finite face displacement");
    }
    field_near(r.top_samples, samples, 0, 1e-14, "top sample selection and order");
}

struct Suite {
    MPI_Comm comm;
    int rank = 0, failures = 0, cases = 0;
    explicit Suite(MPI_Comm c) : comm(c) { MPI_Comm_rank(comm, &rank); }
    void run(const std::string& name, const std::function<void()>& f) {
        int failed = 0;
        std::string why;
        try {
            f();
        } catch (const std::exception& e) {
            failed = 1;
            why = e.what();
        }
        int any = 0;
        MPI_Allreduce(&failed, &any, 1, MPI_INT, MPI_MAX, comm);
        ++cases;
        failures += any;
        if (failed)
            std::cerr << "FAIL [rank " << rank << "] " << name << ": " << why << '\n';
        if (!any && rank == 0)
            std::cout << "PASS " << name << '\n';
    }
    int finish() const {
        if (rank == 0)
            std::cout << cases << " cases, " << failures << " failures\n";
        return failures == 0 ? 0 : 1;
    }
};
// Test geometry is built independently of the solver's gradient and assembly code.
// A union keeps every exterior voxel face, including coplanar segments.
inline ElasticMesh voxel_union(const std::vector<double>& x,
                               const std::vector<double>& y,
                               const std::vector<int>& cell,
                               int ranks = 1) {
    const int nx = static_cast<int>(x.size()) - 1;
    const int ny = static_cast<int>(y.size()) - 1;
    const int count = *std::max_element(cell.begin(), cell.end()) + 1;
    ElasticMesh m;
    m.volume.resize(count);
    m.centroid.resize(count);
    m.gid.resize(count);
    m.layer.assign(count, 0);
    m.rank.resize(count);
    m.is_cut.assign(count, 0);
    m.z_top = 1;
    for (int c = 0; c < count; ++c) {
        m.gid[c] = c;
        m.rank[c] = c % ranks;
    }
    auto owner = [&](int i, int j) {
        return i < 0 || i >= nx || j < 0 || j >= ny ? -1 : cell[i * ny + j];
    };
    for (int i = 0; i < nx; ++i)
        for (int j = 0; j < ny; ++j) {
            const int c = owner(i, j);
            if (c < 0)
                continue;
            const double dx = x[i + 1] - x[i], dy = y[j + 1] - y[j];
            const V midpoint{(x[i] + x[i + 1]) / 2, (y[j] + y[j + 1]) / 2, 0.5};
            m.volume[c] += dx * dy;
            m.centroid[c] = add(m.centroid[c], scale(midpoint, dx * dy));
            for (int d = 0; d < 3; ++d)
                for (int sign : {-1, 1}) {
                    const int other =
                        d == 2 ? -1 : owner(i + (d == 0 ? sign : 0), j + (d == 1 ? sign : 0));
                    if (other == c || (other >= 0 && c > other))
                        continue;
                    ElasticFace f;
                    f.lo = c;
                    f.hi = other;
                    f.normal[d] = sign;
                    f.area = d == 0 ? dy : (d == 1 ? dx : dx * dy);
                    f.centroid = midpoint;
                    f.centroid[d] = d == 0 ? x[i + (sign > 0)]
                                           : (d == 1 ? y[j + (sign > 0)] : (sign > 0 ? 1 : 0));
                    const bool exterior = d == 2 || (d == 0 && (i + sign < 0 || i + sign >= nx)) ||
                                          (d == 1 && (j + sign < 0 || j + sign >= ny));
                    f.side = other < 0 && exterior ? 2 * d + (sign > 0) : -1;
                    m.faces.push_back(f);
                    m.is_embedded.push_back(other < 0 && !exterior ? 1 : 0);
                }
        }
    for (int c = 0; c < count; ++c)
        m.centroid[c] = scale(m.centroid[c], 1 / m.volume[c]);
    return m;
}
inline ElasticMesh concave_l(int ranks = 1) {
    auto m = voxel_union({0, 1, 2, 3}, {0, 1, 2, 3}, {0, 0, 4, 0, 1, 5, 2, 3, 6}, ranks);
    m.is_cut[0] = 1;
    // Use the brief's exact rational centroid, independently checked in the suite.
    m.centroid[0] = {5.0 / 6, 5.0 / 6, 0.5};
    return m;
}
inline ElasticMesh thin_arm_l() {
    auto m = voxel_union({0, 0.2, 1}, {0, 0.2, 1}, {0, 0, 0, -1});
    m.gid[0] = 73;
    m.is_cut[0] = 1;
    m.volume[0] = 9.0 / 25;
    m.centroid[0] = {29.0 / 90, 29.0 / 90, 0.5};
    const auto it = std::find_if(m.faces.begin(), m.faces.end(), [](const auto& f) {
        return f.normal[0] > 0 && std::abs(f.centroid[0] - 0.2) < 1e-15;
    });
    require(it != m.faces.end(), "thin arm notch exists");
    const auto k = static_cast<std::size_t>(it - m.faces.begin());
    std::swap(m.faces[0], m.faces[k]);
    std::swap(m.is_embedded[0], m.is_embedded[k]);
    return m;
}
inline ElasticMesh disconnected_cut(int ranks = 1) {
    auto m = grid(1, 1, 1);
    auto second = grid(1, 1, 1);
    m.volume.push_back(1);
    m.centroid.push_back({2.5, 0.5, 0.5});
    m.gid = {91, 7};
    m.layer.push_back(0);
    m.rank.push_back(ranks > 1 ? 1 : 0);
    m.is_cut = {1, 1};
    for (auto f : second.faces) {
        f.lo = 1;
        f.centroid[0] += 2;
        m.faces.push_back(f);
        m.is_embedded.push_back(0);
    }
    return m;
}
struct GeometryReference {
    double min_rho = std::numeric_limits<double>::infinity();
    std::int64_t min_gid = std::numeric_limits<std::int64_t>::max();
    std::vector<double> moment_defect, closure_defect;
};
inline GeometryReference geometry_reference(const ElasticMesh& m) {
    GeometryReference r;
    std::vector<M3> moment(m.volume.size());
    std::vector<V> area(m.volume.size());
    for (const auto& f : m.faces) {
        const V d = sub(f.hi < 0 ? f.centroid : m.centroid[f.hi], m.centroid[f.lo]);
        require(norm(d) > 0 && std::isfinite(norm(d)), "reference finite nonzero distance");
        const double rho = dot(d, f.normal) / norm(d);
        // Exact ties are intentional: a near tie must never be grouped by tolerance.
        if (rho < r.min_rho || (rho == r.min_rho && m.gid[f.lo] < r.min_gid)) {
            r.min_rho = rho;
            r.min_gid = m.gid[f.lo];
        }
        for (int side = 0; side < (f.hi < 0 ? 1 : 2); ++side) {
            const auto c = side == 0 ? f.lo : f.hi;
            const V s = scale(f.normal, side == 0 ? f.area : -f.area);
            area[c] = add(area[c], s);
            const auto delta = sub(f.centroid, m.centroid[c]);
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j)
                    moment[c][3 * i + j] += s[i] * delta[j];
        }
    }
    for (std::size_t c = 0; c < m.volume.size(); ++c) {
        double defect = 0;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                defect =
                    std::max(defect, std::abs(moment[c][3 * i + j] - (i == j ? m.volume[c] : 0)));
        r.moment_defect.push_back(defect / m.volume[c]);
        r.closure_defect.push_back(norm(area[c]));
    }
    return r;
}
// Convex polygon clipping against n.x <= h. Polygon winding is preserved.
using Polygon = std::vector<V>;
inline Polygon clip_polygon(const Polygon& polygon, const V& n, double h, Polygon& intersections) {
    Polygon out;
    for (std::size_t k = 0; k < polygon.size(); ++k) {
        const V a = polygon[k], b = polygon[(k + 1) % polygon.size()];
        const double da = dot(n, a) - h, db = dot(n, b) - h;
        if (da <= 0)
            out.push_back(a);
        if ((da < 0 && db > 0) || (da > 0 && db < 0)) {
            const V x = add(a, scale(sub(b, a), da / (da - db)));
            out.push_back(x);
            intersections.push_back(x);
        }
    }
    return out;
}
inline std::pair<double, V> polygon_geometry(const Polygon& polygon) {
    double area = 0;
    V moment{};
    for (std::size_t k = 1; k + 1 < polygon.size(); ++k) {
        const double a =
            norm(cross(sub(polygon[k], polygon[0]), sub(polygon[k + 1], polygon[0]))) / 2;
        area += a;
        moment =
            add(moment,
                scale(add(sub(polygon[k], polygon[0]), sub(polygon[k + 1], polygon[0])), a / 3));
    }
    require(area > 0, "nondegenerate clipped polygon");
    return {area, add(polygon[0], scale(moment, 1 / area))};
}
inline ElasticMesh slanted_cut(int ranks = 1) {
    // No grid vertex lies on this plane. It intersects cells in all three directions.
    const V n = scale(V{1, 0.37, 0.29}, 1 / norm(V{1, 0.37, 0.29}));
    const double h = 2.2 / norm(V{1, 0.37, 0.29});
    struct FacePolygon {
        Polygon polygon;
        int side;
        V normal;
        bool embedded;
    };
    ElasticMesh m;
    m.z_top = 2;
    std::map<std::array<int, 4>, std::size_t> shared;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            for (int k = 0; k < 2; ++k) {
                const V lo{static_cast<double>(i), static_cast<double>(j), static_cast<double>(k)};
                const V hi = add(lo, {1, 1, 1});
                if (dot(n, lo) > h)
                    continue;
                std::vector<FacePolygon> faces;
                Polygon intersections;
                for (int d = 0; d < 3; ++d)
                    for (int sign : {-1, 1}) {
                        const int a = (d + 1) % 3, b = (d + 2) % 3;
                        Polygon p(4, lo);
                        for (auto& v : p)
                            v[d] = sign < 0 ? lo[d] : hi[d];
                        p[1][a] = p[2][a] = hi[a];
                        p[2][b] = p[3][b] = hi[b];
                        if (sign < 0)
                            std::reverse(p.begin(), p.end());
                        p = clip_polygon(p, n, h, intersections);
                        if (p.size() >= 3) {
                            V normal{};
                            normal[d] = sign;
                            faces.push_back({p, 2 * d + (sign > 0), normal, false});
                        }
                    }
                Polygon cap;
                for (const auto& x : intersections)
                    if (std::none_of(cap.begin(), cap.end(), [&](const V& q) {
                            return norm(sub(x, q)) < 1e-13;
                        }))
                        cap.push_back(x);
                if (cap.size() >= 3) {
                    V c{};
                    for (const auto& x : cap)
                        c = add(c, x);
                    c = scale(c, 1 / static_cast<double>(cap.size()));
                    const V u = scale(cross(n, {0, 0, 1}), 1 / norm(cross(n, {0, 0, 1})));
                    const V v = cross(n, u);
                    std::sort(cap.begin(), cap.end(), [&](const V& a, const V& b) {
                        return std::atan2(dot(sub(a, c), v), dot(sub(a, c), u)) <
                               std::atan2(dot(sub(b, c), v), dot(sub(b, c), u));
                    });
                    faces.push_back({cap, -1, n, true});
                }
                // Signed tetrahedral moments about an interior vertex average avoid cancellation.
                V origin{};
                std::size_t vertices = 0;
                for (const auto& f : faces)
                    for (const auto& x : f.polygon) {
                        origin = add(origin, x);
                        ++vertices;
                    }
                origin = scale(origin, 1 / static_cast<double>(vertices));
                double volume = 0;
                V moment{};
                for (const auto& f : faces)
                    for (std::size_t q = 1; q + 1 < f.polygon.size(); ++q) {
                        const V a = f.polygon[0], b = f.polygon[q], c = f.polygon[q + 1];
                        const double vol =
                            dot(sub(a, origin), cross(sub(b, origin), sub(c, origin))) / 6;
                        volume += vol;
                        moment = add(moment,
                                     scale(add(add(sub(a, origin), sub(b, origin)), sub(c, origin)),
                                           vol / 4));
                    }
                require(volume > 0, "positive clipped volume");
                const auto cell = static_cast<std::int64_t>(m.volume.size());
                m.volume.push_back(volume);
                m.centroid.push_back(add(origin, scale(moment, 1 / volume)));
                m.gid.push_back(4001 - 31 * cell);
                m.layer.push_back(k);
                m.rank.push_back((i * 4 + j) % ranks);
                m.is_cut.push_back(cap.empty() ? 0 : 1);
                for (const auto& f : faces) {
                    const auto [area, centroid] = polygon_geometry(f.polygon);
                    int side = f.side;
                    std::array<int, 4> key{};
                    if (!f.embedded) {
                        const int d = side / 2;
                        const bool positive = side % 2 != 0;
                        key = {d, i, j, k};
                        key[d + 1] += positive ? 1 : 0;
                        const auto it = shared.find(key);
                        if (it != shared.end()) {
                            auto& existing = m.faces[it->second];
                            existing.hi = cell;
                            existing.side = -1;
                            near(existing.area, area, 1e-14, 1e-14, "shared polygon area");
                            require(norm(sub(existing.centroid, centroid)) <= 1e-13,
                                    "shared polygon centroid");
                            continue;
                        }
                        shared.emplace(key, m.faces.size());
                    }
                    m.faces.push_back({cell, -1, side, area, f.normal, centroid});
                    m.is_embedded.push_back(f.embedded ? 1 : 0);
                }
            }
    return m;
}

struct Suite;
// Also linked into the parent's np2 executable, so MPI acceptance uses its existing mode.
void concave_acceptance(Suite&, int ranks, bool mpi_only = false);
} // namespace elastic_test
