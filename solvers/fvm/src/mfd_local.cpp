#include "mfd_local.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
namespace chipcae::solvers::fvm::detail {
namespace {
mesh::Error failure(const FvGrid& g, std::size_t c, const std::string& reason) {
    return {"fvm/mfd",
            "INTERNAL: gid=" + std::to_string(g.cv_elem_gid.at(c)) + " subcell=" +
                std::to_string(c < g.cv_subcell.size() ? g.cv_subcell[c] : 0) + " " + reason};
}
} // namespace
Expected<double> geometry_defect(const FvGrid& g, std::size_t c) {
    if (c >= g.cv_volume.size() || c >= g.cv_elem_gid.size())
        return mesh::Error{"fvm/mfd", "INTERNAL: invalid CV index"};
    if (g.cv_face_off.size() != g.cv_volume.size() + 1 ||
        g.cv_centroid.size() != g.cv_volume.size() || g.cv_face_off[c] < 0 ||
        g.cv_face_off[c + 1] < g.cv_face_off[c] ||
        static_cast<std::size_t>(g.cv_face_off[c + 1]) > g.cv_faces.size())
        return failure(g, c, "invalid CSR");
    const long double v = g.cv_volume[c];
    if (!(v > 0) || !std::isfinite(v))
        return failure(g, c, "invalid volume");
    long double nr[3][3]{};
    for (auto j = g.cv_face_off[c]; j < g.cv_face_off[c + 1]; ++j) {
        auto id = g.cv_faces[j];
        if (id < 0 || static_cast<std::size_t>(id) >= g.faces.size())
            return failure(g, c, "invalid face");
        const auto& f = g.faces[id];
        if ((f.lo != static_cast<std::int64_t>(c) && f.hi != static_cast<std::int64_t>(c)) ||
            !(f.area > 0) || !std::isfinite(f.area))
            return failure(g, c, "invalid face area/incidence");
        for (int a = 0; a < 3; ++a)
            for (int b = 0; b < 3; ++b) {
                if (!std::isfinite(f.area_vector[a]) || !std::isfinite(f.centroid[b]) ||
                    !std::isfinite(g.cv_centroid[c][b]))
                    return failure(g, c, "nonfinite geometry");
                nr[a][b] += (f.lo == static_cast<std::int64_t>(c) ? 1 : -1) *
                            static_cast<long double>(f.area_vector[a]) *
                            (static_cast<long double>(f.centroid[b]) - g.cv_centroid[c][b]);
            }
    }
    long double defect = 0;
    for (int a = 0; a < 3; ++a)
        for (int b = 0; b < 3; ++b)
            defect = std::max(defect, std::abs(nr[a][b] - (a == b ? v : 0)) / v);
    if (defect > 1e-10L) {
        std::ostringstream s;
        s << std::setprecision(17) << "geometry defect=" << defect;
        return failure(g, c, s.str());
    }
    return static_cast<double>(defect);
}
Dense LocalMfd::apply(const Dense& rhs) const {
    Dense x = rhs;
    const auto n = b.size();
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = 0; j < i; ++j)
            x[i] -= L[i * n + j] * x[j];
        x[i] /= L[i * n + i];
    }
    for (std::size_t i = n; i-- > 0;) {
        for (std::size_t j = i + 1; j < n; ++j)
            x[i] -= L[j * n + i] * x[j];
        x[i] /= L[i * n + i];
    }
    return x;
}
Expected<LocalMfd> build_local_mfd(const FvGrid& g,
                                   std::size_t c,
                                   const std::array<double, 3>& k,
                                   const std::string& mode) {
    auto gate = geometry_defect(g, c);
    if (!gate)
        return gate.error();
    for (auto x : k)
        if (!(x > 0) || !std::isfinite(x))
            return failure(g, c, "invalid conductivity");
    if (mode != "diag" && mode != "scalar")
        return failure(g, c, "invalid stabilization");
    const auto n = static_cast<std::size_t>(g.cv_face_off[c + 1] - g.cv_face_off[c]);
    const long double v = g.cv_volume[c];
    Dense r(n * 3), Q(n * 3), p(n * n), m0(n * n), gamma(n);
    for (std::size_t i = 0; i < n; ++i) {
        const auto& f = g.faces[g.cv_faces[g.cv_face_off[c] + static_cast<std::int64_t>(i)]];
        for (int d = 0; d < 3; ++d) {
            r[i * 3 + d] = static_cast<long double>(f.centroid[d]) - g.cv_centroid[c][d];
            Q[i * 3 + d] = (f.lo == static_cast<std::int64_t>(c) ? 1 : -1) *
                           static_cast<long double>(f.area_vector[d]);
        }
    }
    // Reorthogonalized thin QR; no geometric centre-distance sign assumptions.
    for (int d = 0; d < 3; ++d) {
        for (int pass = 0; pass < 2; ++pass)
            for (int e = 0; e < d; ++e) {
                long double dot = 0;
                for (std::size_t i = 0; i < n; ++i)
                    dot += Q[i * 3 + d] * Q[i * 3 + e];
                for (std::size_t i = 0; i < n; ++i)
                    Q[i * 3 + d] -= dot * Q[i * 3 + e];
            }
        long double norm = 0;
        for (std::size_t i = 0; i < n; ++i)
            norm += Q[i * 3 + d] * Q[i * 3 + d];
        if (!(norm > 0))
            return failure(g, c, "QR rank failure");
        norm = std::sqrt(norm);
        for (std::size_t i = 0; i < n; ++i)
            Q[i * 3 + d] /= norm;
    }
    long double trace = 0, max_gamma = 0;
    for (std::size_t i = 0; i < n; ++i)
        for (std::size_t j = 0; j < n; ++j) {
            p[i * n + j] = (i == j ? 1 : 0);
            for (int d = 0; d < 3; ++d) {
                p[i * n + j] -= Q[i * 3 + d] * Q[j * 3 + d];
                m0[i * n + j] += r[i * 3 + d] * r[j * 3 + d] / (v * k[d]);
            }
            if (i == j) {
                trace += m0[i * n + i];
                gamma[i] = 2 * m0[i * n + i];
                max_gamma = std::max(max_gamma, gamma[i]);
            }
        }
    LocalMfd out;
    for (auto& x : gamma)
        if (x < 1e-8L * max_gamma) {
            x = 1e-8L * max_gamma;
            if (mode == "diag")
                ++out.gamma_floor_count;
        }
    Dense m = m0;
    for (std::size_t i = 0; i < n; ++i)
        for (std::size_t j = 0; j <= i; ++j) {
            long double z = m0[i * n + j];
            if (mode == "scalar")
                z += trace / 3 * p[i * n + j];
            else
                for (std::size_t a = 0; a < n; ++a)
                    z += p[i * n + a] * gamma[a] * p[a * n + j];
            m[i * n + j] = m[j * n + i] = z;
        }
    out.M.assign(m.begin(), m.end());
    out.L.assign(n * n, 0);
    out.b.assign(n, 1);
    for (std::size_t i = 0; i < n; ++i)
        for (std::size_t j = 0; j <= i; ++j) {
            auto z = m[i * n + j];
            for (std::size_t a = 0; a < j; ++a)
                z -= out.L[i * n + a] * out.L[j * n + a];
            if (i == j) {
                if (!(z > 0) || !std::isfinite(z))
                    return failure(g, c, "Cholesky failure");
                out.L[i * n + j] = std::sqrt(z);
            } else
                out.L[i * n + j] = z / out.L[j * n + j];
        }
    // Symmetric Jacobi eigenvalues without spectral clipping.
    Dense eigen = m;
    for (std::size_t sweep = 0; sweep < 100 * n * n; ++sweep) {
        std::size_t a = 0, b = 0;
        long double largest = 0, scale = 0;
        for (std::size_t i = 0; i < n; ++i) {
            scale = std::max(scale, std::abs(eigen[i * n + i]));
            for (std::size_t j = i + 1; j < n; ++j)
                if (std::abs(eigen[i * n + j]) > largest) {
                    largest = std::abs(eigen[i * n + j]);
                    a = i;
                    b = j;
                }
        }
        if (largest <= 1e-19L * scale)
            break;
        const auto theta =
            .5L * std::atan2(2 * eigen[a * n + b], eigen[b * n + b] - eigen[a * n + a]);
        const auto cs = std::cos(theta), sn = std::sin(theta);
        const auto aa = eigen[a * n + a], bb = eigen[b * n + b], ab = eigen[a * n + b];
        for (std::size_t i = 0; i < n; ++i)
            if (i != a && i != b) {
                const auto x = eigen[i * n + a], y = eigen[i * n + b];
                eigen[i * n + a] = eigen[a * n + i] = cs * x - sn * y;
                eigen[i * n + b] = eigen[b * n + i] = sn * x + cs * y;
            }
        eigen[a * n + a] = cs * cs * aa - 2 * cs * sn * ab + sn * sn * bb;
        eigen[b * n + b] = sn * sn * aa + 2 * cs * sn * ab + cs * cs * bb;
        eigen[a * n + b] = eigen[b * n + a] = 0;
    }
    long double emin = eigen[0], emax = eigen[0];
    for (std::size_t i = 0; i < n; ++i) {
        emin = std::min(emin, eigen[i * n + i]);
        emax = std::max(emax, eigen[i * n + i]);
    }
    if (!(emin > 0))
        return failure(g, c, "nonpositive symmetric eigenvalue");
    out.condition = static_cast<double>(emax / emin);
    out.b = out.apply(out.b);
    for (auto x : out.b)
        out.alpha += x;
    if (!(out.alpha > 0))
        return failure(g, c, "nonpositive condensation alpha");
    out.S.resize(n * n);
    for (std::size_t j = 0; j < n; ++j) {
        Dense unit(n, 0);
        unit[j] = 1;
        auto col = out.apply(unit);
        for (std::size_t i = 0; i < n; ++i)
            out.S[i * n + j] = col[i] - out.b[i] * out.b[j] / out.alpha;
    }
    // Preserve the constant nullspace under floating point assembly.
    for (std::size_t i = 0; i < n; ++i) {
        long double sum = 0;
        for (std::size_t j = 0; j < n; ++j)
            if (i != j)
                sum += out.S[i * n + j];
        out.S[i * n + i] = -sum;
    }
    return out;
}
} // namespace chipcae::solvers::fvm::detail
