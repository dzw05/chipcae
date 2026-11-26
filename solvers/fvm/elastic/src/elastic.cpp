#include "chipcae/solvers/fvm/elastic.hpp"

#include "elastic_stencil.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <numeric>
#include <petscksp.h>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace chipcae::solvers::fvm {
namespace detail {
namespace {
using V = ElasticVector;
using M = ElasticMatrix;
using C6 = std::array<double, 36>;
using TMap = std::array<double, 27>;
using Index = std::int64_t;
struct Invalid : std::runtime_error {
    using std::runtime_error::runtime_error;
};
void require(bool ok, const std::string& why) {
    if (!ok)
        throw Invalid(why);
}
double dot(const V& a, const V& b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
V subtract(const V& a, const V& b) {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}
V cross(const V& a, const V& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
template <class A>
bool nonzero(const A& a) {
    return std::any_of(a.begin(), a.end(), [](double x) { return x != 0; });
}
template <class A>
bool finite(const A& a) {
    return std::all_of(a.begin(), a.end(), [](double x) { return std::isfinite(x); });
}
// Diagonal equilibration makes the Cholesky decision independent of physical units.
bool spd(const C6& c) {
    C6 l{};
    for (int i = 0; i < 6; ++i)
        if (!(c[6 * i + i] > 0))
            return false;
    for (int i = 0; i < 6; ++i)
        for (int j = 0; j <= i; ++j) {
            const double scale = std::sqrt(c[6 * i + i]) * std::sqrt(c[6 * j + j]);
            if (std::abs(c[6 * i + j] - c[6 * j + i]) > 1e-12 * scale)
                return false;
            double v = c[6 * i + j] / scale;
            for (int k = 0; k < j; ++k)
                v -= l[6 * i + k] * l[6 * j + k];
            if (i == j) {
                if (!(v > 1e-14))
                    return false;
                l[6 * i + j] = std::sqrt(v);
            } else
                l[6 * i + j] = v / l[6 * j + j];
        }
    return true;
}
bool cut_cell(const ElasticMesh& m, Index c) {
    return !m.is_cut.empty() && m.is_cut[static_cast<std::size_t>(c)] != 0;
}
bool cut_face(const ElasticMesh& m, std::size_t k) {
    const auto& f = m.faces[k];
    return m.is_embedded[k] || cut_cell(m, f.lo) || (f.hi >= 0 && cut_cell(m, f.hi));
}
[[noreturn]] void geometry_error(const ElasticMesh& m,
                                 Index c,
                                 const std::string& why,
                                 std::size_t face = std::numeric_limits<std::size_t>::max()) {
    std::ostringstream message;
    message << "INTERNAL: gid=" << m.gid[c];
    if (face != std::numeric_limits<std::size_t>::max())
        message << " face=" << face;
    message << " " << why;
    throw std::runtime_error(message.str());
}
ElasticGeometry geometry(const ElasticMesh& m, MPI_Comm comm) {
    ElasticGeometry out;
    double minimum = std::numeric_limits<double>::infinity();
    std::vector<double> rho(m.faces.size());
    std::vector<M> moment(m.volume.size());
    std::vector<V> closure(m.volume.size());
    std::vector<double> area(m.volume.size());
    for (std::size_t k = 0; k < m.faces.size(); ++k) {
        const auto& f = m.faces[k];
        const V d = subtract(f.hi < 0 ? f.centroid : m.centroid[f.hi], m.centroid[f.lo]);
        const double distance = std::sqrt(dot(d, d));
        if (!std::isfinite(distance) || !(distance > 0))
            geometry_error(m, f.lo, "zero/nonfinite centroid distance", k);
        rho[k] = dot(d, f.normal) / distance;
        if (!std::isfinite(rho[k]))
            geometry_error(m, f.lo, "nonfinite rho", k);
        if (cut_face(m, k) && rho[k] <= 1e-6) {
            std::ostringstream why;
            why << std::setprecision(17) << "rho=" << rho[k] << " <= 1e-6";
            geometry_error(m, f.lo, why.str(), k);
        }
        minimum = std::min(minimum, rho[k]);
        for (int side = 0; side < (f.hi < 0 ? 1 : 2); ++side) {
            const Index c = side == 0 ? f.lo : f.hi;
            if (!cut_cell(m, c))
                continue;
            const V delta = subtract(f.centroid, m.centroid[c]);
            area[c] += f.area;
            for (int i = 0; i < 3; ++i) {
                const double sf = (side == 0 ? 1 : -1) * f.area * f.normal[i];
                closure[c][i] += sf;
                for (int j = 0; j < 3; ++j)
                    moment[c][3 * i + j] += sf * delta[j];
            }
        }
    }
    // Keep integer gids out of floating point reductions, including gids above 2^53.
    MPI_Allreduce(&minimum, &out.min_dn_ratio, 1, MPI_DOUBLE, MPI_MIN, comm);
    Index gid = std::numeric_limits<Index>::max();
    for (std::size_t k = 0; k < m.faces.size(); ++k)
        if (rho[k] == out.min_dn_ratio)
            gid = std::min(gid, m.gid[m.faces[k].lo]);
    MPI_Allreduce(&gid, &out.min_dn_gid, 1, MPI_INT64_T, MPI_MIN, comm);
    double defect = 0;
    for (std::size_t c = 0; c < m.volume.size(); ++c)
        if (cut_cell(m, static_cast<Index>(c))) {
            double error = 0, closure_error = 0;
            for (int i = 0; i < 3; ++i) {
                closure_error = std::max(closure_error, std::abs(closure[c][i]) / area[c]);
                for (int j = 0; j < 3; ++j)
                    error = std::max(
                        error, std::abs(moment[c][3 * i + j] / m.volume[c] - (i == j ? 1 : 0)));
            }
            if (!std::isfinite(error) || error > 1e-10 || !std::isfinite(closure_error) ||
                closure_error > 1e-10) {
                std::ostringstream why;
                why << std::setprecision(17) << "identity defect=" << error
                    << " area-vector closure defect=" << closure_error;
                geometry_error(m, static_cast<Index>(c), why.str());
            }
            defect = std::max(defect, error);
        }
    MPI_Allreduce(&defect, &out.max_identity_defect, 1, MPI_DOUBLE, MPI_MAX, comm);
    return out;
}
ElasticBoundary validate(const ElasticMesh& m,
                         const ElasticProblem& p,
                         MPI_Comm comm,
                         ElasticGeometry* diagnostics = nullptr) {
    int ranks = 0;
    require(comm != MPI_COMM_NULL && MPI_Comm_size(comm, &ranks) == MPI_SUCCESS,
            "invalid communicator");
    const auto n = m.volume.size(), nf = m.faces.size();
    require(n > 0 && n <= static_cast<std::size_t>(std::numeric_limits<PetscInt>::max() / 3),
            "cell count");
    require(n <= static_cast<std::size_t>(std::numeric_limits<int>::max() / 3),
            "MPI count exceeds int");
    require(m.centroid.size() == n && m.gid.size() == n && m.layer.size() == n &&
                m.rank.size() == n && p.C.size() == n && p.alpha.size() == n && p.dT.size() == n,
            "cell array lengths");
    require(nf > 0 && nf <= static_cast<std::size_t>(std::numeric_limits<int>::max() / 3) &&
                m.is_embedded.size() == nf,
            "face array lengths/count");
    require(m.is_cut.empty() || m.is_cut.size() == n, "is_cut length");
    require(std::all_of(
                m.is_cut.begin(), m.is_cut.end(), [](char flag) { return flag == 0 || flag == 1; }),
            "is_cut must be binary");
    require(std::isfinite(m.z_top), "nonfinite z_top");
    require(p.preconditioner == "jacobi" || p.preconditioner == "gamg",
            "preconditioner must be jacobi or gamg");
    require(std::isfinite(p.rtol) && p.rtol > 0 && p.rtol < 1 && p.max_iter > 0,
            "solver tolerances");
    require(p.support == ElasticProblem::Support::Fixed ||
                p.support == ElasticProblem::Support::Rigid321,
            "support");
    std::set<Index> gids;
    for (std::size_t c = 0; c < n; ++c) {
        if (!std::isfinite(m.volume[c]) || !(m.volume[c] > 0))
            geometry_error(m, static_cast<Index>(c), "nonpositive/nonfinite volume");
        require(m.layer[c] >= 0 && m.rank[c] >= 0 && m.rank[c] < ranks, "cell layer/owner");
        require(gids.insert(m.gid[c]).second, "duplicate gid");
        require(finite(p.C[c]) && spd(p.C[c]), "C must be finite, symmetric positive definite");
        require(finite(p.alpha[c]) && std::isfinite(p.dT[c]), "thermal data");
        for (double a : p.alpha[c])
            require(std::isfinite(a * p.dT[c]), "thermal strain overflow");
    }
    for (std::size_t k = 0; k < nf; ++k) {
        const auto& f = m.faces[k];
        require(f.lo >= 0 && static_cast<std::size_t>(f.lo) < n && f.hi >= -1 &&
                    (f.hi < 0 || (static_cast<std::size_t>(f.hi) < n && f.hi != f.lo)),
                "face cell indices");
        require(f.side >= -1 && f.side <= 5 && (f.hi < 0 || f.side == -1), "face side");
        require((m.is_embedded[k] == 0 || m.is_embedded[k] == 1) &&
                    (!m.is_embedded[k] || (f.hi == -1 && f.side == -1)),
                "embedded face metadata");
        if (!std::isfinite(f.area) || !(f.area > 0) || !finite(f.centroid) ||
            !finite(m.centroid[f.lo]) || (f.hi >= 0 && !finite(m.centroid[f.hi])))
            geometry_error(m, f.lo, "nonpositive/nonfinite face area or centroid geometry", k);
        require(finite(f.normal), "nonfinite normal");
        require(std::abs(dot(f.normal, f.normal) - 1) < 1e-10, "face normal must be unit length");
    }
    const auto measured = geometry(m, comm);
    if (diagnostics)
        *diagnostics = measured;
    for (std::size_t k = 0; k < nf; ++k) {
        if (cut_face(m, k))
            continue;
        const auto& f = m.faces[k];
        const double dl = dot(subtract(f.centroid, m.centroid[f.lo]), f.normal);
        if (dl == 0)
            geometry_error(m, f.lo, "zero projected distance", k);
        require(std::isfinite(dl) && dl > 0, "outward lo projected distance");
        if (f.hi >= 0) {
            const double dh = dot(subtract(m.centroid[f.hi], f.centroid), f.normal);
            if (dh == 0)
                geometry_error(m, f.lo, "zero projected distance", k);
            require(std::isfinite(dh) && dh > 0, "outward hi projected distance");
        }
    }

    ElasticBoundary bc{std::vector<char>(nf), std::vector<V>(nf)};
    const bool fixed = p.support == ElasticProblem::Support::Fixed;
    require(fixed || (p.fixed_side.empty() && p.fixed_layer.empty() && p.prescribed.empty()),
            "mixed Fixed and Rigid321 supports");
    auto set = [&](std::size_t f, const V& u) {
        require(finite(u), "nonfinite prescribed displacement");
        if (bc.prescribed[f])
            require(bc.value[f] == u, "conflicting prescribed displacement");
        bc.prescribed[f] = 1;
        bc.value[f] = u;
    };
    for (int side : p.fixed_side) {
        require(side >= 0 && side <= 5, "fixed side target");
        bool found = false;
        for (std::size_t k = 0; k < nf; ++k)
            if (m.faces[k].hi < 0 && !m.is_embedded[k] && m.faces[k].side == side) {
                set(k, {});
                found = true;
            }
        require(found, "unmatched Fixed side target");
    }
    for (int layer : p.fixed_layer) {
        require(layer >= 0, "fixed layer target");
        bool found = false;
        for (std::size_t k = 0; k < nf; ++k)
            if (m.faces[k].hi < 0 && !m.is_embedded[k] && m.layer[m.faces[k].lo] == layer) {
                set(k, {});
                found = true;
            }
        require(found, "unmatched Fixed layer target");
    }
    for (const auto& b : p.prescribed) {
        require(b.face >= 0 && static_cast<std::size_t>(b.face) < nf, "prescribed face index");
        const auto k = static_cast<std::size_t>(b.face);
        require(m.faces[k].hi < 0 && !m.is_embedded[k],
                "prescribed face must be external and nonembedded");
        set(k, b.U);
    }
    require(!fixed ||
                std::find(bc.prescribed.begin(), bc.prescribed.end(), 1) != bc.prescribed.end(),
            "Fixed requires a displacement boundary");
    return bc;
}
// sigma.n = T(C,n) G. Engineering strains are xx,yy,zz,yz,xz,xy.
TMap traction_map(const C6& c, const V& n) {
    constexpr int strain[3][3] = {{0, 5, 4}, {5, 1, 3}, {4, 3, 2}};
    TMap t{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            for (int a = 0; a < 3; ++a)
                for (int b = 0; b < 3; ++b)
                    t[9 * i + 3 * a + b] += n[j] * c[6 * strain[i][j] + strain[a][b]];
    return t;
}
V thermal_traction(const TMap& t, const ElasticProblem& p, std::size_t c) {
    V v{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            v[i] += t[9 * i + 4 * j] * p.alpha[c][j] * p.dT[c];
    return v;
}
// One-sided Jacobi SVD, followed by a rank-truncated pseudoinverse. No normal
// equations are formed. Column scaling happens before this routine.
std::vector<double> pseudoinverse(std::vector<double> a, int rows, int cols) {
    const auto nr = static_cast<std::size_t>(rows), nc = static_cast<std::size_t>(cols);
    std::vector<double> v(nc * nc);
    for (int i = 0; i < cols; ++i)
        v[nc * i + i] = 1;
    for (int sweep = 0; sweep < 80; ++sweep) {
        bool changed = false;
        for (int p = 0; p < cols; ++p)
            for (int q = p + 1; q < cols; ++q) {
                double aa = 0, bb = 0, ab = 0;
                for (int i = 0; i < rows; ++i) {
                    aa += a[nc * i + p] * a[nc * i + p];
                    bb += a[nc * i + q] * a[nc * i + q];
                    ab += a[nc * i + p] * a[nc * i + q];
                }
                if (aa == 0 || bb == 0 || std::abs(ab) <= 2e-16 * std::sqrt(aa * bb))
                    continue;
                const double tau = (bb - aa) / (2 * ab);
                const double tangent =
                    std::copysign(1.0, tau) / (std::abs(tau) + std::hypot(1.0, tau));
                const double cs = 1 / std::hypot(1.0, tangent), sn = cs * tangent;
                for (int i = 0; i < rows; ++i) {
                    const double x = a[nc * i + p], y = a[nc * i + q];
                    a[nc * i + p] = cs * x - sn * y;
                    a[nc * i + q] = sn * x + cs * y;
                }
                for (int i = 0; i < cols; ++i) {
                    const double x = v[nc * i + p], y = v[nc * i + q];
                    v[nc * i + p] = cs * x - sn * y;
                    v[nc * i + q] = sn * x + cs * y;
                }
                changed = true;
            }
        if (!changed)
            break;
    }
    std::vector<double> norm2(static_cast<std::size_t>(cols));
    for (int j = 0; j < cols; ++j)
        for (int i = 0; i < rows; ++i)
            norm2[j] += a[nc * i + j] * a[nc * i + j];
    const double largest = *std::max_element(norm2.begin(), norm2.end());
    std::vector<double> inverse(nc * nr);
    for (int k = 0; k < cols; ++k)
        if (norm2[k] > largest * 1e-26)
            for (int i = 0; i < cols; ++i)
                for (int j = 0; j < rows; ++j)
                    inverse[nr * i + j] += v[nc * i + k] * a[nc * j + k] / norm2[k];
    return inverse;
}
std::vector<ElasticGradient>
gradients(const ElasticMesh& m, const ElasticProblem& p, const ElasticBoundary& bc) {
    std::vector<std::vector<std::size_t>> incident(m.volume.size());
    for (std::size_t f = 0; f < m.faces.size(); ++f) {
        incident[m.faces[f].lo].push_back(f);
        if (m.faces[f].hi >= 0)
            incident[m.faces[f].hi].push_back(f);
    }
    std::vector<ElasticGradient> out(m.volume.size());
    for (std::size_t c = 0; c < m.volume.size(); ++c) {
        V scale{};
        for (auto k : incident[c]) {
            const auto& f = m.faces[k];
            const Index nb = f.lo == static_cast<Index>(c) ? f.hi : f.lo;
            const V d = subtract(nb < 0 ? f.centroid : m.centroid[nb], m.centroid[c]);
            for (int j = 0; j < 3; ++j)
                scale[j] = std::max(scale[j], std::abs(d[j]));
        }
        for (auto& s : scale) {
            require(s > 0, "cell has no geometric extent");
        }
        std::vector<double> a;
        struct Load {
            std::map<Index, V> blocks, boundary;
            double constant = 0;
        };
        std::vector<Load> rhs;
        for (auto k : incident[c]) {
            const auto& f = m.faces[k];
            const Index nb = f.lo == static_cast<Index>(c) ? f.hi : f.lo;
            if (nb >= 0 || bc.prescribed[k]) {
                V d = subtract(nb < 0 ? f.centroid : m.centroid[nb], m.centroid[c]);
                for (int j = 0; j < 3; ++j)
                    d[j] /= scale[j];
                const double w = 1 / std::sqrt(dot(d, d));
                for (int i = 0; i < 3; ++i) {
                    std::array<double, 9> row{};
                    for (int j = 0; j < 3; ++j)
                        row[3 * i + j] = d[j] * w;
                    a.insert(a.end(), row.begin(), row.end());
                    Load load;
                    load.blocks[static_cast<Index>(c)][i] = -w;
                    if (nb >= 0)
                        load.blocks[nb][i] = w;
                    else {
                        load.constant = w * bc.value[k][i];
                        load.boundary[static_cast<Index>(k)][i] = w;
                    }
                    rhs.push_back(std::move(load));
                }
            } else {
                const auto t = traction_map(p.C[c], f.normal);
                const V thermal = thermal_traction(t, p, c);
                for (int i = 0; i < 3; ++i) {
                    std::array<double, 9> row{};
                    double norm2 = 0;
                    for (int j = 0; j < 9; ++j) {
                        row[j] = t[9 * i + j] / scale[j % 3];
                        norm2 += row[j] * row[j];
                    }
                    const double w = 1 / std::sqrt(norm2);
                    for (auto x : row)
                        a.push_back(x * w);
                    Load load;
                    load.constant = thermal[i] * w;
                    rhs.push_back(std::move(load));
                }
            }
        }
        // A terminal cut cell may have only one center neighbor. Its local
        // traction equations cannot identify rotation about that direction.
        // Complete only this cut prior with a connected geometric patch until
        // displacement samples span space (or the body is exhausted). The final
        // cut operator is still the constrained face-value GG operator below.
        if (cut_cell(m, static_cast<Index>(c))) {
            std::vector<V> directions;
            auto direction = [&](V d) {
                for (int j = 0; j < 3; ++j)
                    d[j] /= scale[j];
                const double original = std::sqrt(dot(d, d));
                if (!(original > 0))
                    return;
                for (auto& v : d)
                    v /= original;
                for (int pass = 0; pass < 2; ++pass)
                    for (const auto& axis : directions) {
                        const double projection = dot(axis, d);
                        for (int j = 0; j < 3; ++j)
                            d[j] -= projection * axis[j];
                    }
                const double norm = std::sqrt(dot(d, d));
                if (norm > 1e-10) {
                    for (auto& v : d)
                        v /= norm;
                    directions.push_back(d);
                }
            };
            std::set<Index> sampled;
            for (auto k : incident[c]) {
                const auto& f = m.faces[k];
                const Index nb = f.lo == static_cast<Index>(c) ? f.hi : f.lo;
                if (nb >= 0) {
                    sampled.insert(nb);
                    direction(subtract(m.centroid[nb], m.centroid[c]));
                } else if (bc.prescribed[k])
                    direction(subtract(f.centroid, m.centroid[c]));
            }
            std::vector<Index> queue{static_cast<Index>(c)};
            std::set<Index> visited{static_cast<Index>(c)};
            for (std::size_t head = 0; head < queue.size() && directions.size() < 3; ++head)
                for (auto k : incident[queue[head]]) {
                    const auto& f = m.faces[k];
                    const Index nb = f.lo == queue[head] ? f.hi : f.lo;
                    if (nb < 0 || !visited.insert(nb).second)
                        continue;
                    queue.push_back(nb);
                    if (sampled.contains(nb))
                        continue;
                    V d = subtract(m.centroid[nb], m.centroid[c]);
                    direction(d);
                    for (int j = 0; j < 3; ++j)
                        d[j] /= scale[j];
                    const double norm = std::sqrt(dot(d, d));
                    if (!(norm > 0))
                        continue;
                    const double w = 1 / norm;
                    for (int i = 0; i < 3; ++i) {
                        std::array<double, 9> row{};
                        for (int j = 0; j < 3; ++j)
                            row[3 * i + j] = d[j] * w;
                        a.insert(a.end(), row.begin(), row.end());
                        Load load;
                        load.blocks[static_cast<Index>(c)][i] = -w;
                        load.blocks[nb][i] = w;
                        rhs.push_back(std::move(load));
                    }
                    if (directions.size() == 3)
                        break;
                }
        }
        require(finite(a), "gradient scaling exceeds floating-point range");
        const int rows = static_cast<int>(rhs.size());
        const auto inv = pseudoinverse(std::move(a), rows, 9);
        auto& g = out[c];
        for (int i = 0; i < 9; ++i)
            for (int r = 0; r < rows; ++r) {
                const double v = inv[static_cast<std::size_t>(rows) * i + r] / scale[i % 3];
                g.constant[i] += v * rhs[r].constant;
                for (const auto& [column, b] : rhs[r].blocks)
                    for (int j = 0; j < 3; ++j)
                        g.blocks[column][3 * i + j] += v * b[j];
                for (const auto& [face, b] : rhs[r].boundary)
                    for (int j = 0; j < 3; ++j)
                        g.boundary[face][3 * i + j] += v * b[j];
            }
    }
    return out;
}
using DofMap = std::map<Index, std::vector<double>>;
struct AffineMap {
    int rows = 0;
    DofMap cells, boundary;
    std::vector<double> constant;
};
AffineMap gradient_map(const ElasticGradient& g) {
    AffineMap a;
    a.rows = 9;
    a.constant.assign(g.constant.begin(), g.constant.end());
    for (const auto& [c, b] : g.blocks)
        a.cells[c] = {b.begin(), b.end()};
    for (const auto& [f, b] : g.boundary)
        a.boundary[f] = {b.begin(), b.end()};
    return a;
}
AffineMap transform(const AffineMap& a, int rows, const std::vector<double>& t) {
    AffineMap b;
    b.rows = rows;
    b.constant.resize(static_cast<std::size_t>(rows));
    for (int i = 0; i < rows; ++i)
        for (int j = 0; j < a.rows; ++j)
            b.constant[i] += t[a.rows * i + j] * a.constant[j];
    auto apply = [&](const DofMap& src, DofMap& dst) {
        for (const auto& [col, value] : src) {
            std::vector<double> out(static_cast<std::size_t>(3 * rows));
            for (int i = 0; i < rows; ++i)
                for (int q = 0; q < 3; ++q)
                    for (int j = 0; j < a.rows; ++j)
                        out[3 * i + q] += t[a.rows * i + j] * value[3 * j + q];
            if (nonzero(out))
                dst.emplace(col, std::move(out));
        }
    };
    apply(a.cells, b.cells);
    apply(a.boundary, b.boundary);
    return b;
}
void add_map(AffineMap& a, const AffineMap& b, double weight) {
    if (a.rows == 0) {
        a.rows = b.rows;
        a.constant.resize(static_cast<std::size_t>(b.rows));
    }
    for (int i = 0; i < a.rows; ++i)
        a.constant[i] += weight * b.constant[i];
    auto add = [&](DofMap& dst, const DofMap& src) {
        for (const auto& [col, value] : src) {
            auto& out = dst[col];
            out.resize(static_cast<std::size_t>(3 * a.rows));
            for (int i = 0; i < 3 * a.rows; ++i)
                out[i] += weight * value[i];
        }
    };
    add(a.cells, b.cells);
    add(a.boundary, b.boundary);
}
AffineMap displacement_map(Index cell) {
    AffineMap a;
    a.rows = 3;
    a.constant.resize(3);
    a.cells[cell] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    return a;
}
AffineMap prescribed_map(std::size_t face, const ElasticBoundary& bc) {
    AffineMap a;
    a.rows = 3;
    a.constant.assign(bc.value[face].begin(), bc.value[face].end());
    a.boundary[static_cast<Index>(face)] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    return a;
}
AffineMap extrapolation(const AffineMap& g, const V& d) {
    std::vector<double> t(27);
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            t[9 * i + 3 * i + j] = d[j];
    return transform(g, 3, t);
}
AffineMap extrapolation(const ElasticGradient& g, const V& d) {
    return extrapolation(gradient_map(g), d);
}
// Shared by production assembly and the face-gradient diagnostic. Keeping the
// full affine operator here includes its tangential and boundary derivatives.
AffineMap compact_face_gradient(
    const AffineMap& lo, const AffineMap& hi, const AffineMap& jump, const V& d, const V& normal) {
    AffineMap mean;
    add_map(mean, lo, hi.rows == 0 ? 1 : 0.5);
    if (hi.rows != 0)
        add_map(mean, hi, 0.5);
    auto residual = jump;
    add_map(residual, extrapolation(mean, d), -1);
    std::vector<double> lift(27);
    const double dn = dot(d, normal);
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            lift[3 * (3 * i + j) + i] = normal[j] / dn;
    add_map(mean, transform(residual, 9, lift), 1);
    return mean;
}
// Euclidean inverse-distance interpolation, including concave owners whose
// individual projected face distances can have either sign.
double cut_lo_weight(const ElasticMesh& m, const ElasticFace& f) {
    const V dl = subtract(f.centroid, m.centroid[f.lo]);
    const V dh = subtract(f.centroid, m.centroid[f.hi]);
    const double l = std::sqrt(dot(dl, dl)), h = std::sqrt(dot(dh, dh));
    require(std::isfinite(l) && std::isfinite(h) && l > 0 && h > 0,
            "INTERNAL: invalid cell-to-face interpolation distance");
    return h / (l + h);
}
AffineMap cut_face_value(const ElasticMesh& m,
                         const ElasticBoundary& bc,
                         const std::vector<ElasticGradient>& g,
                         std::size_t k) {
    if (bc.prescribed[k])
        return prescribed_map(k, bc);
    const auto& f = m.faces[k];
    AffineMap uf;
    const double wl = f.hi < 0 ? 1 : cut_lo_weight(m, f);
    add_map(uf, displacement_map(f.lo), wl);
    add_map(uf, extrapolation(g[f.lo], subtract(f.centroid, m.centroid[f.lo])), wl);
    if (f.hi >= 0) {
        add_map(uf, displacement_map(f.hi), 1 - wl);
        add_map(uf, extrapolation(g[f.hi], subtract(f.centroid, m.centroid[f.hi])), 1 - wl);
    }
    return uf;
}
// Explicit closure: among gradients satisfying G = sum(U_face tensor S)/V,
// minimize the Euclidean change from the parent's local constitutive closure.
// Orthonormal row elimination supplies independent constraints. Dependent
// equations are checked rather than choosing a pseudoinverse for a singular GG
// system. Noncut gradients remain exactly the parent operators.
std::vector<ElasticGradient> cut_gradients(const ElasticMesh& m,
                                           const ElasticBoundary& bc,
                                           const std::vector<ElasticGradient>& prior) {
    std::vector<Index> cells;
    std::vector<int> offset(m.volume.size(), -1);
    for (std::size_t c = 0; c < m.volume.size(); ++c)
        if (cut_cell(m, static_cast<Index>(c))) {
            require(cells.size() < static_cast<std::size_t>(std::numeric_limits<int>::max() / 3),
                    "cut gradient count");
            offset[c] = static_cast<int>(3 * cells.size());
            cells.push_back(static_cast<Index>(c));
        }
    if (cells.empty())
        return prior;
    const auto count = 3 * cells.size();
    std::vector<std::vector<double>> matrix(count, std::vector<double>(count));
    std::vector<AffineMap> rhs(count);
    for (std::size_t r = 0; r < count; ++r) {
        matrix[r][r] = 1;
        rhs[r].rows = 3;
        rhs[r].constant.resize(3);
    }
    auto known = prior;
    for (auto c : cells)
        known[c] = {};
    for (std::size_t k = 0; k < m.faces.size(); ++k) {
        const auto& f = m.faces[k];
        if (!cut_cell(m, f.lo) && (f.hi < 0 || !cut_cell(m, f.hi)))
            continue;
        const auto uf = cut_face_value(m, bc, known, k);
        const double wl = f.hi < 0 ? 1 : cut_lo_weight(m, f);
        for (int side = 0; side < (f.hi < 0 ? 1 : 2); ++side) {
            const Index c = side == 0 ? f.lo : f.hi;
            if (offset[c] < 0)
                continue;
            for (int j = 0; j < 3; ++j) {
                const auto row = static_cast<std::size_t>(offset[c] + j);
                const double sf = (side == 0 ? 1 : -1) * f.area * f.normal[j] / m.volume[c];
                add_map(rhs[row], uf, sf);
                if (bc.prescribed[k])
                    continue;
                for (int neighbor = 0; neighbor < (f.hi < 0 ? 1 : 2); ++neighbor) {
                    const Index nc = neighbor == 0 ? f.lo : f.hi;
                    if (offset[nc] < 0)
                        continue;
                    const double weight = neighbor == 0 ? wl : 1 - wl;
                    const V d = subtract(f.centroid, m.centroid[nc]);
                    for (int q = 0; q < 3; ++q)
                        matrix[row][static_cast<std::size_t>(offset[nc] + q)] -= sf * weight * d[q];
                }
            }
        }
    }
    auto norm_map = [](const AffineMap& map) {
        double n = 0;
        for (double v : map.constant)
            n = std::max(n, std::abs(v));
        for (const auto* terms : {&map.cells, &map.boundary})
            for (const auto& [key, value] : *terms) {
                (void)key;
                for (double v : value)
                    n = std::max(n, std::abs(v));
            }
        return n;
    };
    // Scale threshold by the original equation norm, never by a vanishing
    // residual row. This separates redundant free-face identities from data.
    double matrix_scale = 1, rhs_scale = 1;
    for (std::size_t r = 0; r < count; ++r) {
        matrix_scale = std::max(matrix_scale,
                                std::sqrt(std::inner_product(
                                    matrix[r].begin(), matrix[r].end(), matrix[r].begin(), 0.0)));
        rhs_scale = std::max(rhs_scale, norm_map(rhs[r]));
    }
    std::vector<std::vector<double>> basis;
    std::vector<AffineMap> loads;
    for (std::size_t r = 0; r < count; ++r) {
        auto row = matrix[r];
        auto load = rhs[r];
        for (int pass = 0; pass < 2; ++pass)
            for (std::size_t b = 0; b < basis.size(); ++b) {
                const double projection =
                    std::inner_product(row.begin(), row.end(), basis[b].begin(), 0.0);
                for (std::size_t j = 0; j < count; ++j)
                    row[j] -= projection * basis[b][j];
                add_map(load, loads[b], -projection);
            }
        const double norm = std::sqrt(std::inner_product(row.begin(), row.end(), row.begin(), 0.0));
        if (norm <= 1e-12 * matrix_scale) {
            require(norm_map(load) <= 1e-10 * rhs_scale,
                    "INTERNAL: incompatible Green-Gauss gradient constraints");
            continue;
        }
        for (auto& v : row)
            v /= norm;
        AffineMap scaled;
        add_map(scaled, load, 1 / norm);
        basis.push_back(std::move(row));
        loads.push_back(std::move(scaled));
    }
    auto out = prior;
    for (std::size_t b = 0; b < basis.size(); ++b) {
        AffineMap correction = loads[b];
        for (std::size_t q = 0; q < cells.size(); ++q)
            for (int j = 0; j < 3; ++j) {
                V axis{};
                axis[j] = 1;
                add_map(correction, extrapolation(prior[cells[q]], axis), -basis[b][3 * q + j]);
            }
        for (std::size_t q = 0; q < cells.size(); ++q)
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j) {
                    const double weight = basis[b][3 * q + j];
                    auto& g = out[cells[q]];
                    g.constant[3 * i + j] += weight * correction.constant[i];
                    for (const auto& [col, value] : correction.cells)
                        for (int d = 0; d < 3; ++d)
                            g.blocks[col][3 * (3 * i + j) + d] += weight * value[3 * i + d];
                    for (const auto& [face, value] : correction.boundary)
                        for (int d = 0; d < 3; ++d)
                            g.boundary[face][3 * (3 * i + j) + d] += weight * value[3 * i + d];
                }
    }
    return out;
}
std::vector<ElasticGradient>
reconstruction(const ElasticMesh& m, const ElasticProblem& p, const ElasticBoundary& bc) {
    return cut_gradients(m, bc, gradients(m, p, bc));
}
// Summation-by-parts gradient: V G = sum_f (U_f-U_c) tensor n A.
// A constant stress then passes an all-prescribed affine patch exactly, and
// the adjoint of this gradient defines conservative, moment-balanced forces.
std::vector<AffineMap> gauss_gradients(const ElasticMesh& m,
                                       const ElasticBoundary& bc,
                                       const std::vector<ElasticGradient>& ls) {
    std::vector<AffineMap> out(m.volume.size());
    for (std::size_t k = 0; k < m.faces.size(); ++k) {
        const auto& f = m.faces[k];
        AffineMap uf;
        if (cut_face(m, k))
            uf = cut_face_value(m, bc, ls, k);
        else if (bc.prescribed[k])
            uf = prescribed_map(k, bc);
        else if (f.hi < 0) {
            uf = displacement_map(f.lo);
            add_map(uf, extrapolation(ls[f.lo], subtract(f.centroid, m.centroid[f.lo])), 1);
        } else {
            const double dl = dot(subtract(f.centroid, m.centroid[f.lo]), f.normal);
            const double dh = dot(subtract(m.centroid[f.hi], f.centroid), f.normal);
            const double wl = dh / (dl + dh), wh = dl / (dl + dh);
            add_map(uf, displacement_map(f.lo), wl);
            add_map(uf, displacement_map(f.hi), wh);
            V offset{};
            for (int j = 0; j < 3; ++j)
                offset[j] = f.centroid[j] - m.centroid[f.lo][j] -
                            wh * (m.centroid[f.hi][j] - m.centroid[f.lo][j]);
            // By construction the centroid offset is tangential. Remove the
            // roundoff normal component rather than creating zero-ring fill.
            const double normal_offset = dot(offset, f.normal);
            for (int j = 0; j < 3; ++j)
                offset[j] -= normal_offset * f.normal[j];
            add_map(uf, extrapolation(ls[f.lo], offset), wl);
            add_map(uf, extrapolation(ls[f.hi], offset), wh);
        }
        for (int side = 0; side < (f.hi >= 0 ? 2 : 1); ++side) {
            const Index c = side == 0 ? f.lo : f.hi;
            AffineMap difference = uf;
            add_map(difference, displacement_map(c), -1);
            std::vector<double> t(27);
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j)
                    t[3 * (3 * i + j) + i] = f.normal[j];
            add_map(
                out[c], transform(difference, 9, t), (side == 0 ? 1 : -1) * f.area / m.volume[c]);
        }
    }
    return out;
}
C6 face_stiffness(const ElasticProblem& p, const ElasticFace& f, const ElasticMesh& m, bool cut) {
    C6 cf = p.C[f.lo];
    if (f.hi < 0)
        return cf;
    const double dl = dot(subtract(f.centroid, m.centroid[f.lo]), f.normal);
    const double dh = dot(subtract(m.centroid[f.hi], f.centroid), f.normal);
    const double wl = cut ? cut_lo_weight(m, f) : dh / (dl + dh),
                 wh = cut ? 1 - wl : dl / (dl + dh);
    for (int j = 0; j < 36; ++j) {
        const double a = cf[j], b = p.C[f.hi][j];
        if (a == 0 || b == 0)
            cf[j] = 0;
        else if (std::signbit(a) == std::signbit(b))
            cf[j] = 1 / (wh / a + wl / b);
        else
            cf[j] = wl * a + wh * b;
    }
    require(spd(cf), "harmonic face C is not SPD");
    return cf;
}
ElasticAssembly assemble(const ElasticMesh& m,
                         const ElasticProblem& p,
                         const ElasticBoundary& bc,
                         const std::vector<ElasticGradient>& ls,
                         bool retain_faces = true) {
    ElasticAssembly out;
    out.faces.resize(m.faces.size());
    out.rhs.resize(m.volume.size());
    std::vector<std::map<Index, M>> faces(retain_faces ? m.faces.size() : 0), rows(m.volume.size());
    std::vector<std::vector<std::pair<Index, std::size_t>>> adjacency(m.volume.size());
    for (std::size_t k = 0; k < m.faces.size(); ++k) {
        const auto& f = m.faces[k];
        if (f.hi >= 0) {
            adjacency[f.lo].push_back({f.hi, k});
            adjacency[f.hi].push_back({f.lo, k});
        }
    }
    // Every derivative in an energy term is expressed as center-to-neighbor or
    // center-to-prescribed-face differences. Route longer implicit differences
    // over deterministic shortest graph paths, retaining one physical flux per face.
    auto path = [&](Index from, Index to) {
        std::vector<std::pair<std::size_t, double>> route;
        if (from == to)
            return route;
        std::map<Index, std::pair<Index, std::size_t>> previous;
        std::vector<Index> queue{from};
        previous[from] = {from, 0};
        for (std::size_t i = 0; i < queue.size() && !previous.contains(to); ++i)
            for (const auto& [next, face] : adjacency[queue[i]])
                if (!previous.contains(next)) {
                    previous[next] = {queue[i], face};
                    queue.push_back(next);
                }
        require(previous.contains(to), "disconnected gradient stencil");
        for (Index c = to; c != from;) {
            const auto [prev, k] = previous[c];
            route.push_back({k, m.faces[k].lo == prev ? 1 : -1});
            c = prev;
        }
        return route;
    };
    auto add_flux =
        [&](std::size_t k, double sign, const std::vector<double>& b, const AffineMap& stress) {
            auto& face = out.faces[k];
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < stress.rows; ++j)
                    face.constant[i] += sign * b[3 * j + i] * stress.constant[j];
            for (const auto& [col, value] : stress.cells) {
                M contribution{};
                for (int i = 0; i < 3; ++i)
                    for (int q = 0; q < 3; ++q)
                        for (int j = 0; j < stress.rows; ++j)
                            contribution[3 * i + q] += sign * b[3 * j + i] * value[3 * j + q];
                if (nonzero(contribution)) {
                    auto& block = faces[k][col];
                    for (int j = 0; j < 9; ++j)
                        block[j] += contribution[j];
                }
            }
        };
    auto energy = [&](Index anchor,
                      const AffineMap& strain,
                      const std::vector<double>& coefficient,
                      double weight) {
        auto stress = transform(strain, strain.rows, coefficient);
        for (auto& v : stress.constant)
            v *= weight;
        for (auto& [col, b] : stress.cells) {
            (void)col;
            for (auto& v : b)
                v *= weight;
        }
        // Direct energy Hessian for the production solve; diagnostic fluxes
        // are optional and do not duplicate the entire face stencil in KSP.
        for (const auto& [row, b] : strain.cells) {
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < stress.rows; ++j)
                    out.rhs[row][i] -= b[3 * j + i] * stress.constant[j];
            for (const auto& [column, value] : stress.cells) {
                M contribution{};
                for (int i = 0; i < 3; ++i)
                    for (int q = 0; q < 3; ++q)
                        for (int j = 0; j < stress.rows; ++j)
                            contribution[3 * i + q] += b[3 * j + i] * value[3 * j + q];
                if (nonzero(contribution)) {
                    auto& block = rows[row][column];
                    for (int j = 0; j < 9; ++j)
                        block[j] += contribution[j];
                }
            }
        }
        if (!retain_faces)
            return;
        for (const auto& [cell, b] : strain.cells)
            if (cell != anchor)
                for (const auto& [face, sign] : path(anchor, cell))
                    add_flux(face, sign, b, stress);
        for (const auto& [face, b] : strain.boundary) {
            const Index cell = m.faces[face].lo;
            for (const auto& [k, sign] : path(anchor, cell))
                add_flux(k, sign, b, stress);
            add_flux(static_cast<std::size_t>(face), 1, b, stress);
        }
    };
    auto g = gauss_gradients(m, bc, ls);
    std::vector<double> engineering(54);
    for (int i = 0; i < 3; ++i)
        engineering[9 * i + 4 * i] = 1;
    engineering[9 * 3 + 5] = engineering[9 * 3 + 7] = 1;
    engineering[9 * 4 + 2] = engineering[9 * 4 + 6] = 1;
    engineering[9 * 5 + 1] = engineering[9 * 5 + 3] = 1;
    for (std::size_t c = 0; c < m.volume.size(); ++c) {
        auto e = transform(g[c], 6, engineering);
        for (int j = 0; j < 3; ++j)
            e.constant[j] -= p.alpha[c][j] * p.dT[c];
        energy(static_cast<Index>(c), e, {p.C[c].begin(), p.C[c].end()}, m.volume[c]);
    }
    for (std::size_t k = 0; k < m.faces.size(); ++k) {
        const auto& f = m.faces[k];
        if (f.hi < 0 && !bc.prescribed[k])
            continue;
        const V d = subtract(f.hi < 0 ? f.centroid : m.centroid[f.hi], m.centroid[f.lo]);
        const double dn = dot(d, f.normal);
        AffineMap r = f.hi < 0 ? prescribed_map(k, bc) : displacement_map(f.hi);
        add_map(r, displacement_map(f.lo), -1);
        if (cut_face(m, k)) {
            auto face_gradient =
                compact_face_gradient(gradient_map(ls[f.lo]),
                                      f.hi < 0 ? AffineMap{} : gradient_map(ls[f.hi]),
                                      r,
                                      d,
                                      f.normal);
            // Retain the parent's cell-energy consistency and adjoint traction.
            // Stabilize the full face strain relative to that same consistency
            // gradient, rather than independently penalizing deltaU-Gbar*d.
            // This includes tangential mismatch at cut/uncut interfaces and
            // reduces to the parent acoustic penalty when the gradients agree.
            add_map(face_gradient, g[f.lo], f.hi < 0 ? -1 : -0.5);
            if (f.hi >= 0)
                add_map(face_gradient, g[f.hi], -0.5);
            const auto c = face_stiffness(p, f, m, true);
            energy(
                f.lo, transform(face_gradient, 6, engineering), {c.begin(), c.end()}, f.area * dn);
            continue;
        }
        if (f.hi < 0)
            add_map(r, extrapolation(ls[f.lo], d), -1);
        else {
            const double wl = dot(subtract(m.centroid[f.hi], f.centroid), f.normal) / dn;
            add_map(r, extrapolation(ls[f.lo], d), -wl);
            add_map(r, extrapolation(ls[f.hi], d), -(1 - wl));
        }
        const auto t = traction_map(face_stiffness(p, f, m, false), f.normal);
        std::vector<double> acoustic(9);
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                for (int q = 0; q < 3; ++q)
                    acoustic[3 * i + j] += t[9 * i + 3 * j + q] * f.normal[q];
        energy(f.lo, r, acoustic, f.area / dn);
    }
    g.clear();
    g.shrink_to_fit();
    if (retain_faces)
        for (std::size_t k = 0; k < m.faces.size(); ++k) {
            auto& face = out.faces[k];
            face.blocks.reserve(faces[k].size());
            for (const auto& [col, b] : faces[k])
                if (nonzero(b))
                    face.blocks.push_back({col, b});
            faces[k].clear();
        }
    std::size_t count_blocks = 0;
    for (const auto& row : rows)
        count_blocks += row.size();
    out.blocks.reserve(count_blocks);
    for (std::size_t c = 0; c < rows.size(); ++c) {
        for (const auto& [col, b] : rows[c])
            if (nonzero(b))
                out.blocks.push_back({static_cast<Index>(c), col, b});
        rows[c].clear();
    }
    for (const auto& b : out.blocks)
        require(finite(b.value), "stiffness overflow");
    for (const auto& b : out.rhs)
        require(finite(b), "thermal force overflow");
    return out;
}
} // namespace
Expected<std::vector<ElasticMatrix>> evaluate_cut_gradients(const ElasticMesh& m,
                                                            const ElasticProblem& p,
                                                            const std::vector<V>& u,
                                                            MPI_Comm comm) {
    try {
        const auto bc = validate(m, p, comm);
        require(u.size() == m.volume.size(), "gradient displacement length");
        std::vector<M> out;
        for (const auto& g : reconstruction(m, p, bc)) {
            M value = g.constant;
            for (const auto& [c, block] : g.blocks)
                for (int i = 0; i < 9; ++i)
                    for (int j = 0; j < 3; ++j)
                        value[i] += block[3 * i + j] * u[c][j];
            require(finite(value), "nonfinite gradient displacement");
            out.push_back(value);
        }
        return out;
    } catch (const Invalid& e) {
        return chipcae::mesh::Error{"fvm.elastic", "INPUT_INVALID: " + std::string(e.what())};
    } catch (const std::exception& e) {
        return chipcae::mesh::Error{"fvm.elastic", e.what()};
    }
}
Expected<ElasticMatrix> evaluate_cut_face_gradient(const ElasticMesh& m,
                                                   std::size_t k,
                                                   const std::vector<V>& u,
                                                   const std::vector<M>& g,
                                                   const V& boundary) {
    try {
        require(k < m.faces.size(), "face gradient index");
        const auto& f = m.faces[k];
        const auto n = m.volume.size();
        require(u.size() == n && g.size() == n && m.centroid.size() == n && m.gid.size() == n &&
                    f.lo >= 0 && static_cast<std::size_t>(f.lo) < n && f.hi >= -1 &&
                    (f.hi < 0 || static_cast<std::size_t>(f.hi) < n),
                "face gradient arrays/indices");
        const V d = subtract(f.hi < 0 ? f.centroid : m.centroid[f.hi], m.centroid[f.lo]);
        const double distance = std::sqrt(dot(d, d)), dn = dot(d, f.normal);
        if (!finite(d) || !std::isfinite(distance) || !(distance > 0) || !std::isfinite(dn) ||
            dn / distance <= 1e-6)
            geometry_error(m, f.lo, "invalid compact face gradient distance", k);
        AffineMap lo, hi, jump;
        lo.rows = 9;
        lo.constant.assign(g[f.lo].begin(), g[f.lo].end());
        if (f.hi >= 0) {
            hi.rows = 9;
            hi.constant.assign(g[f.hi].begin(), g[f.hi].end());
        }
        jump.rows = 3;
        const V delta = subtract(f.hi < 0 ? boundary : u[f.hi], u[f.lo]);
        jump.constant.assign(delta.begin(), delta.end());
        const auto gradient = compact_face_gradient(lo, hi, jump, d, f.normal);
        M out{};
        std::copy(gradient.constant.begin(), gradient.constant.end(), out.begin());
        require(finite(out), "nonfinite face gradient");
        return out;
    } catch (const Invalid& e) {
        return chipcae::mesh::Error{"fvm.elastic", "INPUT_INVALID: " + std::string(e.what())};
    } catch (const std::exception& e) {
        return chipcae::mesh::Error{"fvm.elastic", e.what()};
    }
}
Expected<ElasticAssembly>
assemble_elastic(const ElasticMesh& m, const ElasticProblem& p, MPI_Comm comm) {
    try {
        const auto bc = validate(m, p, comm);
        return assemble(m, p, bc, reconstruction(m, p, bc));
    } catch (const Invalid& e) {
        return chipcae::mesh::Error{"fvm.elastic", "INPUT_INVALID: " + std::string(e.what())};
    } catch (const std::exception& e) {
        return chipcae::mesh::Error{"fvm.elastic", e.what()};
    }
}
} // namespace detail
namespace {
using namespace detail;
void petsc(PetscErrorCode code) {
    if (code != PETSC_SUCCESS)
        throw std::runtime_error("PETSc error " + std::to_string(code));
}
template <class T, PetscErrorCode (*Destroy)(T*)>
struct Handle {
    T value = nullptr;
    Handle() = default;
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value(std::exchange(other.value, nullptr)) {}
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) {
            if (value)
                Destroy(&value);
            value = std::exchange(other.value, nullptr);
        }
        return *this;
    }
    ~Handle() {
        if (value)
            Destroy(&value);
    }
    operator T() const { return value; }
};
using MatHandle = Handle<Mat, MatDestroy>;
using VecHandle = Handle<Vec, VecDestroy>;
using KspHandle = Handle<KSP, KSPDestroy>;
using PcHandle = Handle<PC, PCDestroy>;
using NullHandle = Handle<MatNullSpace, MatNullSpaceDestroy>;
using ScatterHandle = Handle<VecScatter, VecScatterDestroy>;
std::vector<std::vector<V>> rigid_basis(const ElasticMesh& m, bool& full_body) {
    const auto n = m.volume.size();
    std::vector<Index> component(n);
    std::iota(component.begin(), component.end(), 0);
    auto find = [&](Index i) {
        while (component[i] != i) {
            component[i] = component[component[i]];
            i = component[i];
        }
        return i;
    };
    for (const auto& f : m.faces)
        if (f.hi >= 0)
            component[find(f.hi)] = find(f.lo);
    std::map<Index, std::vector<std::size_t>> bodies;
    for (std::size_t i = 0; i < n; ++i)
        bodies[find(static_cast<Index>(i))].push_back(i);
    std::vector<std::vector<V>> q;
    for (const auto& [root, cells] : bodies) {
        (void)root;
        V center = m.centroid[cells.front()];
        V offset{};
        for (auto c : cells)
            for (int j = 0; j < 3; ++j)
                offset[j] += (m.centroid[c][j] - center[j]) / static_cast<double>(cells.size());
        for (int j = 0; j < 3; ++j)
            center[j] += offset[j];
        double radius = 0;
        for (auto c : cells) {
            const auto d = subtract(m.centroid[c], center);
            radius = std::max(radius, std::sqrt(dot(d, d)));
        }
        for (int mode = 0; mode < 6; ++mode) {
            std::vector<V> v(n);
            V e{};
            e[mode % 3] = 1;
            for (auto c : cells) {
                v[c] = mode < 3 ? e : cross(e, subtract(m.centroid[c], center));
                if (mode >= 3 && radius > 0)
                    for (auto& x : v[c])
                        x /= radius;
            }
            for (int pass = 0; pass < 2; ++pass)
                for (const auto& basis : q) {
                    double inner = 0;
                    for (auto c : cells)
                        inner += dot(v[c], basis[c]);
                    for (auto c : cells)
                        for (int j = 0; j < 3; ++j)
                            v[c][j] -= inner * basis[c][j];
                }
            double n2 = 0;
            for (auto c : cells)
                n2 += dot(v[c], v[c]);
            if (n2 <= 1e-24 * static_cast<double>(cells.size()))
                continue;
            for (auto c : cells)
                for (auto& x : v[c])
                    x /= std::sqrt(n2);
            q.push_back(std::move(v));
        }
    }
    full_body = bodies.size() == 1 && q.size() == 6;
    return q;
}
void remove_rigid(std::vector<V>& u, const std::vector<std::vector<V>>& q) {
    for (const auto& basis : q) {
        double inner = 0;
        for (std::size_t c = 0; c < u.size(); ++c)
            inner += dot(u[c], basis[c]);
        for (std::size_t c = 0; c < u.size(); ++c)
            for (int j = 0; j < 3; ++j)
                u[c][j] -= inner * basis[c][j];
    }
}
std::array<double, 9> evaluate(const ElasticGradient& g, const std::vector<V>& u) {
    auto out = g.constant;
    for (const auto& [c, b] : g.blocks)
        for (int i = 0; i < 9; ++i)
            for (int j = 0; j < 3; ++j)
                out[i] += b[3 * i + j] * u[c][j];
    return out;
}
void reconstruct(const ElasticMesh& m,
                 const ElasticBoundary& bc,
                 const std::vector<ElasticGradient>& gradients,
                 ElasticResult& r) {
    r.face_prescribed = bc.prescribed;
    r.U_face.resize(m.faces.size());
    std::vector<M> g;
    g.reserve(gradients.size());
    for (const auto& op : gradients)
        g.push_back(evaluate(op, r.U_cell));
    for (std::size_t k = 0; k < m.faces.size(); ++k) {
        const auto& f = m.faces[k];
        V u{};
        if (cut_face(m, k)) {
            const auto face = cut_face_value(m, bc, gradients, k);
            std::copy(face.constant.begin(), face.constant.end(), u.begin());
            for (const auto& [c, value] : face.cells)
                for (int i = 0; i < 3; ++i)
                    for (int j = 0; j < 3; ++j)
                        u[i] += value[3 * i + j] * r.U_cell[c][j];
        } else if (bc.prescribed[k])
            u = bc.value[k];
        else {
            const V dl = subtract(f.centroid, m.centroid[f.lo]);
            V ul = r.U_cell[f.lo];
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j)
                    ul[i] += g[f.lo][3 * i + j] * dl[j];
            u = ul;
            if (f.hi >= 0) {
                const V dh = subtract(m.centroid[f.hi], f.centroid);
                V uh = r.U_cell[f.hi];
                for (int i = 0; i < 3; ++i)
                    for (int j = 0; j < 3; ++j)
                        uh[i] -= g[f.hi][3 * i + j] * dh[j];
                const double wl =
                    dot(dh, f.normal) / dot(subtract(m.centroid[f.hi], m.centroid[f.lo]), f.normal);
                for (int i = 0; i < 3; ++i)
                    u[i] = wl * ul[i] + (1 - wl) * uh[i];
            }
        }
        r.U_face[k] = u;
        if (f.hi < 0 &&
            ((!m.is_embedded[k] && f.side == 5) ||
             (m.is_embedded[k] && f.normal[2] > 0.99 && f.centroid[2] >= m.z_top - 1e-13)))
            r.top_samples.push_back({f.centroid[0], f.centroid[1], u[2]});
    }
    for (const auto& u : r.U_cell)
        r.U_mag_max = std::max(r.U_mag_max, std::sqrt(dot(u, u)));
    const int n = static_cast<int>(r.top_samples.size());
    if (n > 0) {
        V center = r.top_samples.front(), sum{};
        for (const auto& x : r.top_samples)
            for (int j = 0; j < 2; ++j)
                sum[j] += (x[j] - center[j]) / n;
        for (int j = 0; j < 2; ++j)
            center[j] += sum[j];
        V scale{};
        for (const auto& x : r.top_samples)
            for (int j = 0; j < 2; ++j)
                scale[j] = std::max(scale[j], std::abs(x[j] - center[j]));
        std::vector<double> a;
        for (const auto& x : r.top_samples) {
            a.push_back(1);
            for (int j = 0; j < 2; ++j)
                a.push_back(scale[j] > 0 ? (x[j] - center[j]) / scale[j] : 0);
        }
        const auto inv = pseudoinverse(a, n, 3);
        V coefficients{};
        for (int j = 0; j < 3; ++j)
            for (int i = 0; i < n; ++i)
                coefficients[j] += inv[n * j + i] * r.top_samples[i][2];
        double lo = std::numeric_limits<double>::infinity(), hi = -lo;
        for (int i = 0; i < n; ++i) {
            double v = r.top_samples[i][2];
            for (int j = 0; j < 3; ++j)
                v -= a[3 * i + j] * coefficients[j];
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
        r.warpage_pv = hi - lo;
    }
    r.warpage_method = "FVM top-face centroids; least-squares plane removal";
    r.U_mag_method = "FVM cell centers";
}
// A small algebraic Ritz solve supplies a polynomial initial iterate. This is
// particularly useful for long thin bodies: Jacobi alone damps bending modes
// slowly. No beam equations or reference solution enter this coarse space.
struct RitzSpace {
    static constexpr int max_terms = 27;
    int terms = 10, columns = 0;
    std::vector<int> group;
    std::vector<std::array<double, max_terms>> phi;
    std::vector<double> inverse;
    std::vector<double> solve(const std::vector<double>& rhs) const {
        std::vector<double> x(static_cast<std::size_t>(columns));
        for (int i = 0; i < columns; ++i)
            for (int j = 0; j < columns; ++j)
                x[i] += inverse[columns * i + j] * rhs[j];
        return x;
    }
    V value(std::size_t cell, const std::vector<double>& coefficients) const {
        V v{};
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < terms; ++j)
                v[i] += phi[cell][j] * coefficients[3 * terms * group[cell] + terms * i + j];
        return v;
    }
};
RitzSpace ritz_space(const ElasticMesh& m, const ElasticAssembly& a) {
    RitzSpace space;
    std::set<int> layers(m.layer.begin(), m.layer.end());
    std::map<int, int> layer_index;
    int index = 0;
    for (int layer : layers)
        layer_index[layer] = index++;
    const int groups = std::min(index, 8);
    const auto n = m.volume.size();
    auto& group = space.group;
    group.resize(n);
    V lo = m.centroid.front(), hi = lo;
    for (const auto& x : m.centroid)
        for (int j = 0; j < 3; ++j) {
            lo[j] = std::min(lo[j], x[j]);
            hi[j] = std::max(hi[j], x[j]);
        }
    V extent = subtract(hi, lo);
    const int longitudinal =
        static_cast<int>(std::max_element(extent.begin(), extent.end()) - extent.begin());
    const int transverse1 = (longitudinal + 1) % 3, transverse2 = (longitudinal + 2) % 3;
    if (extent[longitudinal] > 4 * std::max(extent[transverse1], extent[transverse2]))
        space.terms = std::min(RitzSpace::max_terms, 240 / (3 * groups));
    const int terms = space.terms, columns = 3 * terms * groups;
    space.columns = columns;
    auto& phi = space.phi;
    phi.resize(n);
    for (std::size_t c = 0; c < n; ++c) {
        group[c] = static_cast<int>(static_cast<Index>(layer_index[m.layer[c]]) * groups / index);
        V x{};
        for (int j = 0; j < 3; ++j)
            x[j] = hi[j] > lo[j] ? (2 * (m.centroid[c][j] - lo[j]) / (hi[j] - lo[j]) - 1) : 0;
        phi[c] = {1,
                  x[0],
                  x[1],
                  x[2],
                  x[0] * x[0],
                  x[1] * x[1],
                  x[2] * x[2],
                  x[0] * x[1],
                  x[0] * x[2],
                  x[1] * x[2]};
        // Geometry-selected polynomial enrichment resolves slowly varying
        // bending modes without an analytic beam solution or material model.
        int next = 10;
        double prev = 1, current = x[longitudinal];
        for (int degree = 2; degree <= 7 && next < terms; ++degree) {
            const double value =
                ((2 * degree - 1) * x[longitudinal] * current - (degree - 1) * prev) / degree;
            if (degree > 2 && next < terms)
                phi[c][next++] = value;
            if (next < terms)
                phi[c][next++] = value * x[transverse1];
            if (next < terms)
                phi[c][next++] = value * x[transverse2];
            prev = current;
            current = value;
        }
    }
    std::vector<double> coarse(static_cast<std::size_t>(columns * columns));
    // Form Phi^T*(A*Phi) a row at a time. This avoids a quadratic
    // polynomial-basis loop for every sparse block and keeps memory bounded.
    std::size_t block_index = 0;
    std::vector<double> product(static_cast<std::size_t>(3 * columns));
    for (std::size_t row = 0; row < n; ++row) {
        std::fill(product.begin(), product.end(), 0);
        while (block_index < a.blocks.size() &&
               a.blocks[block_index].row == static_cast<Index>(row)) {
            const auto& b = a.blocks[block_index++];
            const int base = 3 * terms * group[b.column];
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j) {
                    const double value = b.value[3 * i + j];
                    for (int k = 0; k < terms; ++k)
                        product[columns * i + base + terms * j + k] += value * phi[b.column][k];
                }
        }
        const int base = 3 * terms * group[row];
        for (int i = 0; i < 3; ++i)
            for (int k = 0; k < terms; ++k)
                for (int j = 0; j < columns; ++j)
                    coarse[columns * (base + terms * i + k) + j] +=
                        phi[row][k] * product[columns * i + j];
    }
    std::vector<double> scale(static_cast<std::size_t>(columns));
    for (int i = 0; i < columns; ++i)
        for (int j = 0; j < columns; ++j)
            scale[i] = std::max(scale[i], std::abs(coarse[columns * i + j]));
    for (auto& v : scale)
        v = v > 0 ? std::sqrt(v) : 1;
    for (int i = 0; i < columns; ++i) {
        for (int j = 0; j < columns; ++j)
            coarse[columns * i + j] /= scale[i] * scale[j];
    }
    require(finite(coarse), "coarse matrix scaling exceeds floating-point range");
    space.inverse = pseudoinverse(std::move(coarse), columns, columns);
    for (int i = 0; i < columns; ++i)
        for (int j = 0; j < columns; ++j)
            space.inverse[columns * i + j] /= scale[i] * scale[j];
    for (int i = 0; i < columns; ++i)
        for (int j = 0; j < i; ++j) {
            const double v = (space.inverse[columns * i + j] + space.inverse[columns * j + i]) / 2;
            space.inverse[columns * i + j] = space.inverse[columns * j + i] = v;
        }
    return space;
}
// Balanced two-level acceleration around a Jacobi or GAMG base PC. With
// E=Z^T A Z, it applies Z E^+ Z^T + (I-Z E^+ Z^T A) M^-1
// (I-A Z E^+ Z^T). The Krylov operator remains the physical sparse AIJ.
struct CoarseCorrection {
    Mat physical = nullptr;
    PC base = nullptr;
    MPI_Comm comm = MPI_COMM_NULL;
    const RitzSpace* space = nullptr;
    std::vector<std::size_t> cells;
    bool ready = false, amg = false;
    MatNullSpace rigid = nullptr;
    VecHandle coarse, projected, product;
    std::vector<double> coefficients(Vec v) const {
        const PetscScalar* values = nullptr;
        petsc(VecGetArrayRead(v, &values));
        std::vector<double> load(static_cast<std::size_t>(space->columns));
        for (std::size_t c = 0; c < cells.size(); ++c) {
            const auto cell = cells[c];
            const int base_index = 3 * space->terms * space->group[cell];
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < space->terms; ++j)
                    load[base_index + space->terms * i + j] +=
                        space->phi[cell][j] * PetscRealPart(values[3 * c + i]);
        }
        petsc(VecRestoreArrayRead(v, &values));
        if (MPI_Allreduce(MPI_IN_PLACE, load.data(), space->columns, MPI_DOUBLE, MPI_SUM, comm) !=
            MPI_SUCCESS)
            throw std::runtime_error("MPI coarse reduction failed");
        return space->solve(load);
    }
    void add(Vec v, const std::vector<double>& x, double sign) const {
        PetscScalar* values = nullptr;
        petsc(VecGetArray(v, &values));
        for (std::size_t c = 0; c < cells.size(); ++c) {
            const auto u = space->value(cells[c], x);
            for (int i = 0; i < 3; ++i)
                values[3 * c + i] += sign * u[i];
        }
        petsc(VecRestoreArray(v, &values));
    }
    static PetscErrorCode apply(PC shell, Vec b, Vec x) {
        CoarseCorrection* self = nullptr;
        PetscErrorCode code = PCShellGetContext(shell, &self);
        if (code)
            return code;
        try {
            if (!self->ready) {
                petsc(PCSetUp(self->base));
                if (self->amg) {
                    KSP coarse_ksp = nullptr;
                    petsc(PCMGGetCoarseSolve(self->base, &coarse_ksp));
                    PC coarse_pc = nullptr;
                    petsc(KSPGetPC(coarse_ksp, &coarse_pc));
                    PetscBool block = PETSC_FALSE;
                    petsc(PetscObjectTypeCompare(
                        reinterpret_cast<PetscObject>(coarse_pc), PCBJACOBI, &block));
                    if (block) {
                        PetscInt count = 0, first = 0;
                        KSP* sub = nullptr;
                        petsc(PCBJacobiGetSubKSP(coarse_pc, &count, &first, &sub));
                        for (PetscInt i = 0; i < count; ++i) {
                            Mat mat = nullptr;
                            petsc(KSPGetOperators(sub[i], &mat, nullptr));
                            PetscInt size = 0;
                            petsc(MatGetSize(mat, &size, nullptr));
                            if (size > 0) {
                                PC pc = nullptr;
                                petsc(KSPGetPC(sub[i], &pc));
                                petsc(PCSetType(pc, PCSVD));
                            }
                        }
                    }
                }
                self->ready = true;
            }
            const auto initial = self->coefficients(b);
            petsc(VecSet(self->coarse, 0));
            self->add(self->coarse, initial, 1);
            petsc(MatMult(self->physical, self->coarse, self->product));
            petsc(VecWAXPY(self->projected, -1, self->product, b));
            petsc(PCApply(self->base, self->projected, x));
            if (self->rigid)
                petsc(MatNullSpaceRemove(self->rigid, x));
            petsc(MatMult(self->physical, x, self->product));
            const auto correction = self->coefficients(self->product);
            self->add(x, correction, -1);
            self->add(x, initial, 1);
            if (self->rigid)
                petsc(MatNullSpaceRemove(self->rigid, x));
            return PETSC_SUCCESS;
        } catch (...) {
            return PETSC_ERR_LIB;
        }
    }
};
ElasticResult solve_system(const ElasticMesh& m,
                           const ElasticProblem& p,
                           MPI_Comm comm,
                           const ElasticBoundary& bc,
                           const std::vector<ElasticGradient>& gradients,
                           const ElasticAssembly& assembly) {
    int rank = 0, ranks = 0;
    MPI_Comm_rank(comm, &rank);
    MPI_Comm_size(comm, &ranks);
    const auto n = m.volume.size();
    std::vector<std::size_t> order(n);
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](auto a, auto b) {
        return std::pair{m.rank[a], m.gid[a]} < std::pair{m.rank[b], m.gid[b]};
    });
    std::vector<PetscInt> permutation(n);
    std::vector<PetscInt> count(static_cast<std::size_t>(ranks));
    for (std::size_t i = 0; i < n; ++i) {
        permutation[order[i]] = static_cast<PetscInt>(i);
        ++count[m.rank[order[i]]];
    }
    const PetscInt first = std::accumulate(count.begin(), count.begin() + rank, PetscInt{0}),
                   local = count[rank];
    std::vector<PetscInt> diagonal(static_cast<std::size_t>(3 * local)), off(diagonal.size());
    for (const auto& block : assembly.blocks)
        if (m.rank[block.row] == rank) {
            const auto c = permutation[block.column];
            auto& target = (c >= first && c < first + local) ? diagonal : off;
            for (int j = 0; j < 3; ++j)
                target[3 * (permutation[block.row] - first) + j] += 3;
        }
    MatHandle a;
    VecHandle x, b, physical, coordinates;
    NullHandle nullspace, near;
    petsc(MatCreateAIJ(comm,
                       3 * local,
                       3 * local,
                       static_cast<PetscInt>(3 * n),
                       static_cast<PetscInt>(3 * n),
                       0,
                       diagonal.data(),
                       0,
                       off.data(),
                       &a.value));
    petsc(MatSetBlockSize(a, 3));
    petsc(MatSetOption(a, MAT_IGNORE_ZERO_ENTRIES, PETSC_TRUE));
    for (const auto& block : assembly.blocks)
        if (m.rank[block.row] == rank) {
            const PetscInt row = permutation[block.row], col = permutation[block.column];
            petsc(MatSetValuesBlocked(a, 1, &row, 1, &col, block.value.data(), INSERT_VALUES));
        }
    petsc(MatAssemblyBegin(a, MAT_FINAL_ASSEMBLY));
    petsc(MatAssemblyEnd(a, MAT_FINAL_ASSEMBLY));
    petsc(MatCreateVecs(a, &x.value, &b.value));
    petsc(VecDuplicate(b, &physical.value));
    PetscScalar* values = nullptr;
    petsc(VecGetArray(b, &values));
    for (PetscInt i = 0; i < local; ++i)
        for (int j = 0; j < 3; ++j)
            values[3 * i + j] = assembly.rhs[order[first + i]][j];
    petsc(VecRestoreArray(b, &values));
    petsc(VecCopy(b, physical));
    petsc(VecSet(x, 0));
    std::vector<std::vector<V>> q;
    // Prescribed faces restrain rigid motions. Fixed operators must not carry
    // either exact rigid nullspaces or rigid near-nullspaces for GAMG.
    if (p.support == ElasticProblem::Support::Rigid321) {
        bool full_body = false;
        q = rigid_basis(m, full_body);
        std::vector<VecHandle> modes(q.size());
        std::vector<Vec> raw(q.size());
        for (std::size_t k = 0; k < q.size(); ++k) {
            petsc(VecDuplicate(x, &modes[k].value));
            raw[k] = modes[k];
            petsc(VecGetArray(modes[k], &values));
            for (PetscInt i = 0; i < local; ++i)
                for (int j = 0; j < 3; ++j)
                    values[3 * i + j] = q[k][order[first + i]][j];
            petsc(VecRestoreArray(modes[k], &values));
        }
        petsc(MatNullSpaceCreate(
            comm, PETSC_FALSE, static_cast<PetscInt>(raw.size()), raw.data(), &nullspace.value));
        petsc(VecDuplicate(x, &coordinates.value));
        petsc(VecSetBlockSize(coordinates, 3));
        petsc(VecGetArray(coordinates, &values));
        for (PetscInt i = 0; i < local; ++i)
            for (int j = 0; j < 3; ++j)
                values[3 * i + j] = m.centroid[order[first + i]][j];
        petsc(VecRestoreArray(coordinates, &values));
        if (full_body) {
            petsc(MatNullSpaceCreateRigidBody(coordinates, &near.value));
            petsc(MatSetNearNullSpace(a, near));
        } else
            petsc(MatSetNearNullSpace(a, nullspace));
        petsc(MatSetNullSpace(a, nullspace));
        // Verify the transpose basis before projecting; right null modes alone
        // do not establish compatibility for a nonsymmetric operator.
        VecHandle work;
        petsc(VecDuplicate(b, &work.value));
        PetscReal anorm = 0;
        petsc(MatNorm(a, NORM_FROBENIUS, &anorm));
        bool left = true, right = true;
        for (const auto mode : raw) {
            petsc(MatMult(a, mode, work));
            PetscReal defect = 0;
            petsc(VecNorm(work, NORM_2, &defect));
            if (defect > 1e-11 * anorm)
                right = false;
            petsc(MatMultTranspose(a, mode, work));
            PetscReal v = 0;
            petsc(VecNorm(work, NORM_2, &v));
            if (v > 1e-11 * anorm)
                left = false;
        }
        if (!left || !right)
            throw std::runtime_error("stencil right/transpose rigid nullspace verification failed");
        PetscReal load_norm = 0;
        petsc(VecNorm(b, NORM_2, &load_norm));
        for (const auto mode : raw) {
            PetscScalar inner = 0;
            petsc(VecDot(mode, b, &inner));
            if (PetscAbsScalar(inner) > 1e-11 * load_norm)
                throw std::runtime_error("thermal load incompatible with transpose rigid modes");
        }
        petsc(MatSetTransposeNullSpace(a, nullspace));
        petsc(MatNullSpaceRemove(nullspace, b));
    }
    const auto coarse = ritz_space(m, assembly);
    PcHandle base;
    CoarseCorrection correction;
    KspHandle ksp;
    correction.physical = a;
    correction.comm = comm;
    correction.space = &coarse;
    if (p.support == ElasticProblem::Support::Rigid321)
        correction.rigid = nullspace;
    for (PetscInt i = 0; i < local; ++i)
        correction.cells.push_back(order[first + i]);
    petsc(VecDuplicate(b, &correction.coarse.value));
    petsc(VecDuplicate(b, &correction.projected.value));
    petsc(VecDuplicate(b, &correction.product.value));
    const auto initial = correction.coefficients(b);
    correction.add(x, initial, 1);
    // A constant thermal eigenstrain is an exactly integrable affine warm start,
    // even across stiffness jumps. Verify convergence through the physical KSP.
    bool uniform = p.support == ElasticProblem::Support::Rigid321;
    V strain{};
    for (int j = 0; j < 3; ++j)
        strain[j] = p.alpha.front()[j] * p.dT.front();
    for (std::size_t c = 0; c < n; ++c)
        for (int j = 0; j < 3; ++j)
            uniform = uniform && p.alpha[c][j] * p.dT[c] == strain[j];
    if (uniform) {
        std::vector<V> warm(n);
        V center = m.centroid.front();
        V offset{};
        for (const auto& point : m.centroid)
            for (int j = 0; j < 3; ++j)
                offset[j] += (point[j] - center[j]) / static_cast<double>(n);
        for (int j = 0; j < 3; ++j)
            center[j] += offset[j];
        for (std::size_t c = 0; c < n; ++c)
            for (int j = 0; j < 3; ++j)
                warm[c][j] = strain[j] * (m.centroid[c][j] - center[j]);
        remove_rigid(warm, q);
        petsc(VecGetArray(x, &values));
        for (PetscInt i = 0; i < local; ++i)
            for (int j = 0; j < 3; ++j)
                values[3 * i + j] = warm[order[first + i]][j];
        petsc(VecRestoreArray(x, &values));
    }
    if (p.support == ElasticProblem::Support::Rigid321)
        petsc(MatNullSpaceRemove(nullspace, x));
    PetscReal bnorm = 0;
    petsc(VecNorm(physical, NORM_2, &bnorm));
    petsc(KSPCreate(comm, &ksp.value));
    petsc(KSPSetOperators(ksp, a, a));
    petsc(KSPSetType(ksp, p.preconditioner == "gamg" ? KSPFGMRES : KSPCG));
    petsc(KSPSetInitialGuessNonzero(ksp, PETSC_TRUE));
    if (p.preconditioner == "gamg") {
        petsc(KSPGMRESSetRestart(ksp, 50));
#if PETSC_VERSION_GE(3, 26, 0)
        petsc(
            KSPOrthogonalizationSetCGSRefinementType(ksp, KSP_ORTHOGONALIZATION_CGS_REFINE_ALWAYS));
#else
        petsc(KSPGMRESSetCGSRefinementType(ksp, KSP_GMRES_CGS_REFINE_ALWAYS));
#endif
        petsc(KSPSetPCSide(ksp, PC_RIGHT));
    } else
        petsc(KSPSetPCSide(ksp, PC_LEFT));
    petsc(KSPSetNormType(ksp, KSP_NORM_UNPRECONDITIONED));
    PetscReal anorm = 0, xnorm = 0;
    petsc(MatNorm(a, NORM_INFINITY, &anorm));
    petsc(VecNorm(x, NORM_2, &xnorm));
    // A double-valued displacement cannot generally attain an arbitrarily small
    // relative force residual in a slender body. Keep the requested relative
    // criterion and an explicit floating-point backward-error floor.
    require(std::isfinite(anorm) && std::isfinite(xnorm) && std::isfinite(bnorm),
            "solver scaling exceeds floating-point range");
    const double floor = 4 * std::numeric_limits<double>::epsilon() * anorm * xnorm;
    petsc(KSPSetTolerances(
        ksp, p.rtol, std::max({1e-30, p.rtol * bnorm, floor}), PETSC_DEFAULT, p.max_iter));
    petsc(PCCreate(comm, &base.value));
    petsc(PCSetOperators(base, a, a));
    petsc(PCSetType(base, p.preconditioner == "gamg" ? PCGAMG : PCJACOBI));
    // GAMG reads the normalized Mat near-nullspace made from centroid
    // coordinates above. Raw PCSetCoordinates would override that basis.
    correction.base = base;
    correction.amg = p.preconditioner == "gamg";
    PC pc = nullptr;
    petsc(KSPGetPC(ksp, &pc));
    petsc(PCSetType(pc, PCSHELL));
    petsc(PCShellSetContext(pc, &correction));
    petsc(PCShellSetApply(pc, CoarseCorrection::apply));
    petsc(PCShellSetName(pc, p.preconditioner == "gamg" ? "Ritz + GAMG" : "Ritz + Jacobi"));
    KSPConvergedReason reason{};
    PetscInt its = 0;
    VecHandle residual;
    petsc(VecDuplicate(b, &residual.value));
    PetscReal norm = 0;
    bool converged = false;
    // Reliable restarts prevent an estimated Krylov residual from being
    // mistaken for convergence after cancellation in a two-level PC.
    for (int refinement = 0; refinement < 6 && its < p.max_iter; ++refinement) {
        petsc(VecNorm(x, NORM_2, &xnorm));
        const double atol = std::max(
            {1e-30, p.rtol * bnorm, 4 * std::numeric_limits<double>::epsilon() * anorm * xnorm});
        petsc(KSPSetTolerances(ksp, p.rtol, atol, PETSC_DEFAULT, p.max_iter - its));
        petsc(KSPSolve(ksp, b, x));
        PetscInt count_this = 0;
        petsc(KSPGetConvergedReason(ksp, &reason));
        petsc(KSPGetIterationNumber(ksp, &count_this));
        its += count_this;
        if (p.support == ElasticProblem::Support::Rigid321)
            petsc(MatNullSpaceRemove(nullspace, x));
        petsc(MatMult(a, x, residual));
        petsc(VecAXPY(residual, -1, physical));
        petsc(VecNorm(residual, NORM_2, &norm));
        petsc(VecNorm(x, NORM_2, &xnorm));
        const double attainable = std::max(
            {1e-30, p.rtol * bnorm, 8 * std::numeric_limits<double>::epsilon() * anorm * xnorm});
        if (reason > 0 && std::isfinite(norm) && std::isfinite(xnorm) && norm <= attainable) {
            converged = true;
            break;
        }
        if (reason <= 0 || count_this == 0)
            break;
    }
    if (!converged) {
        PetscReal krylov = 0;
        petsc(KSPGetResidualNorm(ksp, &krylov));
        std::ostringstream why;
        why << "Krylov residual=" << krylov << " KSP failure reason=" << static_cast<int>(reason)
            << " iterations=" << its << " physical residual=" << norm
            << " FVM elastic nonconvergence";
        throw std::runtime_error(why.str());
    }
    VecHandle gathered;
    ScatterHandle scatter;
    petsc(VecScatterCreateToAll(x, &scatter.value, &gathered.value));
    petsc(VecScatterBegin(scatter, x, gathered, INSERT_VALUES, SCATTER_FORWARD));
    petsc(VecScatterEnd(scatter, x, gathered, INSERT_VALUES, SCATTER_FORWARD));
    const PetscScalar* solution = nullptr;
    petsc(VecGetArrayRead(gathered, &solution));
    ElasticResult r;
    r.U_cell.resize(n);
    r.iterations = static_cast<int>(its);
    r.residual = norm;
    for (std::size_t i = 0; i < n; ++i)
        for (int j = 0; j < 3; ++j)
            r.U_cell[order[i]][j] = PetscRealPart(solution[3 * i + j]);
    petsc(VecRestoreArrayRead(gathered, &solution));
    if (p.support == ElasticProblem::Support::Rigid321)
        remove_rigid(r.U_cell, q);
    reconstruct(m, bc, gradients, r);
    return r;
}
} // namespace
Expected<ElasticResult>
solve_elastic(const ElasticMesh& m, const ElasticProblem& p, MPI_Comm comm) {
    try {
        detail::ElasticGeometry measured;
        const auto bc = detail::validate(m, p, comm, &measured);
        const auto g = detail::reconstruction(m, p, bc);
        auto result = solve_system(m, p, comm, bc, g, detail::assemble(m, p, bc, g, false));
        result.min_dn_ratio = measured.min_dn_ratio;
        result.min_dn_gid = measured.min_dn_gid;
        result.max_identity_defect = measured.max_identity_defect;
        return result;
    } catch (const detail::Invalid& e) {
        return chipcae::mesh::Error{"fvm.elastic", "INPUT_INVALID: " + std::string(e.what())};
    } catch (const std::exception& e) {
        return chipcae::mesh::Error{"fvm.elastic", e.what()};
    }
}
} // namespace chipcae::solvers::fvm
