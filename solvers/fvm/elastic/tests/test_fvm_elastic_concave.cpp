#include "elastic_stencil.hpp"
#include "elastic_test_mesh.hpp"

#include <iomanip>
#include <petscksp.h>
#include <petsclog.h>
#include <regex>
#include <sstream>
#include <tuple>

namespace elastic_test {
namespace {
constexpr M3 patch_a{0.002, -0.0007, 0.0011, 0.0003, -0.0013, 0.0009, -0.0004, 0.0006, 0.0008};
constexpr V patch_b{0.0017, -0.0008, 0.0002};
std::vector<V> values(const std::vector<V>& x, const M3& a, const V& b) {
    std::vector<V> u;
    for (const auto& p : x)
        u.push_back(add(mul(a, p), b));
    return u;
}
std::vector<V> face_centers(const ElasticMesh& m) {
    std::vector<V> x;
    for (const auto& f : m.faces)
        x.push_back(f.centroid);
    return x;
}
double matrix_norm(const M3& a) {
    double sum = 0;
    for (double v : a)
        sum += v * v;
    return std::sqrt(sum);
}
void gradient_near(const M3& got, const M3& want, double tolerance, const std::string& label) {
    M3 error{};
    for (int j = 0; j < 9; ++j) {
        require(std::isfinite(got[j]), label + " finite gradient");
        error[j] = got[j] - want[j];
    }
    require(matrix_norm(error) <= tolerance * matrix_norm(want),
            label + " relative error <= " + std::to_string(tolerance));
}
void fixture_geometry(const ElasticMesh& m) {
    const auto g = geometry_reference(m);
    std::cout << std::setprecision(17) << "fixture min_dn_ratio=" << g.min_rho
              << " min_dn_gid=" << g.min_gid << " max_identity_defect="
              << *std::max_element(g.moment_defect.begin(), g.moment_defect.end()) << '\n';
    for (std::size_t c = 0; c < m.volume.size(); ++c) {
        near(g.moment_defect[c],
             0,
             1e-14,
             0,
             "independent moment identity, gid=" + std::to_string(m.gid[c]));
        near(g.closure_defect[c],
             0,
             1e-14 * std::pow(m.volume[c], 2.0 / 3),
             0,
             "independent area-vector closure, gid=" + std::to_string(m.gid[c]));
    }
}
void diagnostics(const ElasticMesh& m, const ElasticResult& r, MPI_Comm comm) {
    const auto g = geometry_reference(m);
    double rho_min = 0, rho_max = 0;
    std::int64_t gid_min = 0, gid_max = 0;
    MPI_Allreduce(&r.min_dn_ratio, &rho_min, 1, MPI_DOUBLE, MPI_MIN, comm);
    MPI_Allreduce(&r.min_dn_ratio, &rho_max, 1, MPI_DOUBLE, MPI_MAX, comm);
    MPI_Allreduce(&r.min_dn_gid, &gid_min, 1, MPI_INT64_T, MPI_MIN, comm);
    MPI_Allreduce(&r.min_dn_gid, &gid_max, 1, MPI_INT64_T, MPI_MAX, comm);
    // Exact equality here is the specified deterministic reduction contract.
    require(rho_min == rho_max && gid_min == gid_max, "identical minimum rho/gid on every rank");
    near(r.min_dn_ratio, g.min_rho, 2e-15, 0, "minimum over all faces");
    require(r.min_dn_gid == g.min_gid, "strict minimum rho, then smallest lo gid");
    double defect = 0;
    for (std::size_t c = 0; c < m.volume.size(); ++c)
        if (!m.is_cut.empty() && m.is_cut[c])
            defect = std::max(defect, g.moment_defect[c]);
    near(r.max_identity_defect, defect, 2e-15, 0, "reported normalized cut identity defect");
    require(r.gradient_corrections >= 0 && r.gradient_corrections <= 50,
            "correction count is zero for full implicit or at most 50");
    std::cout << std::setprecision(17) << "实测 min_dn_ratio=" << r.min_dn_ratio
              << " min_dn_gid=" << r.min_dn_gid << " max_identity_defect=" << r.max_identity_defect
              << " gradient_corrections=" << r.gradient_corrections
              << " iterations=" << r.iterations << " reported_residual=" << r.residual << '\n';
}
detail::ElasticAssembly assemble(const ElasticMesh& m, const ElasticProblem& p) {
    auto a = detail::assemble_elastic(m, p, MPI_COMM_SELF);
    require(static_cast<bool>(a), a ? "" : "diagnostic assembly: " + a.error().reason);
    require(a.value().faces.size() == m.faces.size(), "traction stencil retains face order");
    return std::move(a.value());
}
std::vector<V> tractions(const detail::ElasticAssembly& a, const std::vector<V>& u) {
    std::vector<V> force;
    for (const auto& f : a.faces) {
        auto v = f.constant;
        for (const auto& b : f.blocks) {
            require(b.column >= 0 && static_cast<std::size_t>(b.column) < u.size(),
                    "traction column");
            v = add(v, mul(b.value, u[b.column]));
        }
        force.push_back(v);
    }
    return force;
}
V stress_traction(const ElasticProblem& p, const M3& g, const ElasticFace& f) {
    std::array<double, 6> e{g[0], g[4], g[8], g[5] + g[7], g[2] + g[6], g[1] + g[3]}, s{};
    for (int i = 0; i < 6; ++i) {
        if (i < 3)
            e[i] -= p.alpha[0][i] * p.dT[0];
    }
    for (int i = 0; i < 6; ++i)
        for (int j = 0; j < 6; ++j)
            s[i] += p.C[0][6 * i + j] * e[j];
    return scale({s[0] * f.normal[0] + s[5] * f.normal[1] + s[4] * f.normal[2],
                  s[5] * f.normal[0] + s[1] * f.normal[1] + s[3] * f.normal[2],
                  s[4] * f.normal[0] + s[3] * f.normal[1] + s[2] * f.normal[2]},
                 f.area);
}
void physical_residual(const ElasticMesh& m,
                       const ElasticProblem& p,
                       const ElasticResult& r,
                       double tolerance) {
    const auto a = assemble(m, p);
    auto residual = a.rhs;
    double rhs2 = 0, action2 = 0;
    for (auto& v : residual) {
        rhs2 += dot(v, v);
        v = scale(v, -1);
    }
    std::vector<V> action(m.volume.size());
    for (const auto& b : a.blocks)
        action[b.row] = add(action[b.row], mul(b.value, r.U_cell[b.column]));
    for (std::size_t c = 0; c < residual.size(); ++c) {
        residual[c] = add(residual[c], action[c]);
        action2 += dot(action[c], action[c]);
    }
    double residual2 = 0;
    for (const auto& v : residual)
        residual2 += dot(v, v);
    const double normalized = std::sqrt(residual2) / std::max(std::sqrt(rhs2), std::sqrt(action2));
    require(std::isfinite(normalized) && normalized <= tolerance,
            "corrected physical residual, independently evaluated from production assembly");
    std::cout << "实测 corrected_physical_residual=" << normalized << '\n';
}
ElasticProblem nonaffine_problem(const ElasticMesh& m) {
    auto p = problem(m, laminate(0.3));
    prescribe(p, m, patch_a, patch_b);
    for (auto& prescribed : p.prescribed) {
        const auto& x = m.faces[prescribed.face].centroid;
        prescribed.U =
            add(prescribed.U, {0.0003 * x[1] * x[1], -0.0002 * x[0] * x[2], 0.0004 * x[0] * x[1]});
    }
    for (std::size_t c = 0; c < m.volume.size(); ++c) {
        p.dT[c] += 7 * m.centroid[c][0];
        for (auto& value : p.C[c])
            value *= 1 + 0.1 * m.centroid[c][1];
    }
    return p;
}
// Independent scalar energy oracle. In particular, cut stabilization uses the
// full prescribed face gradient relative to the cell-energy (Gauss) gradients,
// including tangential components at interfaces between cut and uncut cells.
double
face_gradient_energy(const ElasticMesh& m, const ElasticProblem& p, const std::vector<V>& u) {
    const auto reconstructed = detail::evaluate_cut_gradients(m, p, u, MPI_COMM_SELF);
    require(static_cast<bool>(reconstructed), reconstructed ? "" : reconstructed.error().reason);
    const auto& ls = reconstructed.value();
    std::map<std::int64_t, V> prescribed;
    for (const auto& value : p.prescribed)
        prescribed[value.face] = value.U;
    auto cut = [&](std::size_t k) {
        const auto& f = m.faces[k];
        return m.is_embedded[k] || m.is_cut[f.lo] || (f.hi >= 0 && m.is_cut[f.hi]);
    };
    std::vector<M3> gauss(m.volume.size());
    for (std::size_t k = 0; k < m.faces.size(); ++k) {
        const auto& f = m.faces[k];
        const V dl = sub(f.centroid, m.centroid[f.lo]);
        V uf = prescribed.contains(k) ? prescribed.at(k) : add(u[f.lo], mul(ls[f.lo], dl));
        if (f.hi >= 0) {
            const V dh = sub(f.centroid, m.centroid[f.hi]);
            const double wl = cut(k) ? norm(dh) / (norm(dl) + norm(dh))
                                     : -dot(dh, f.normal) / dot(sub(dl, dh), f.normal);
            if (cut(k))
                uf = add(scale(add(u[f.lo], mul(ls[f.lo], dl)), wl),
                         scale(add(u[f.hi], mul(ls[f.hi], dh)), 1 - wl));
            else {
                V tangent = add(scale(dl, wl), scale(dh, 1 - wl));
                tangent = sub(tangent, scale(f.normal, dot(tangent, f.normal)));
                uf = add(
                    add(scale(u[f.lo], wl), scale(u[f.hi], 1 - wl)),
                    add(scale(mul(ls[f.lo], tangent), wl), scale(mul(ls[f.hi], tangent), 1 - wl)));
            }
        }
        for (int side = 0; side < (f.hi >= 0 ? 2 : 1); ++side) {
            const auto c = side == 0 ? f.lo : f.hi;
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j)
                    gauss[c][3 * i + j] += (uf[i] - u[c][i]) * f.normal[j] * f.area *
                                           (side == 0 ? 1 : -1) / m.volume[c];
        }
    }
    auto density = [](const M3& g, const M6& stiffness, const V& thermal = V{}) {
        const std::array<double, 6> e{g[0] - thermal[0],
                                      g[4] - thermal[1],
                                      g[8] - thermal[2],
                                      g[5] + g[7],
                                      g[2] + g[6],
                                      g[1] + g[3]};
        double value = 0;
        for (int i = 0; i < 6; ++i)
            for (int j = 0; j < 6; ++j)
                value += 0.5 * e[i] * stiffness[6 * i + j] * e[j];
        return value;
    };
    double value = 0;
    for (std::size_t c = 0; c < m.volume.size(); ++c)
        value += m.volume[c] * density(gauss[c], p.C[c], scale(p.alpha[c], p.dT[c]));
    for (std::size_t k = 0; k < m.faces.size(); ++k) {
        const auto& f = m.faces[k];
        if (f.hi < 0 && !prescribed.contains(k))
            continue;
        const V d = sub(f.hi < 0 ? f.centroid : m.centroid[f.hi], m.centroid[f.lo]);
        const double dn = dot(d, f.normal);
        const V jump = sub(f.hi < 0 ? prescribed.at(k) : u[f.hi], u[f.lo]);
        M3 defect{};
        M6 stiffness = p.C[f.lo];
        double wl = 1;
        if (f.hi >= 0) {
            const V dl = sub(f.centroid, m.centroid[f.lo]);
            const V dh = sub(m.centroid[f.hi], f.centroid);
            wl = cut(k) ? norm(dh) / (norm(dl) + norm(dh)) : dot(dh, f.normal) / dn;
            // The fixture scales a common SPD tensor, so harmonic interpolation
            // is independently a single scalar harmonic mean, including couplings.
            const double ratio = p.C[f.hi][0] / stiffness[0];
            for (auto& entry : stiffness)
                entry /= (1 - wl) + wl / ratio;
        }
        M3 mean = ls[f.lo];
        if (f.hi >= 0)
            for (int j = 0; j < 9; ++j)
                mean[j] = (cut(k) ? 0.5 : wl) * ls[f.lo][j] + (cut(k) ? 0.5 : 1 - wl) * ls[f.hi][j];
        const V residual = sub(jump, mul(mean, d));
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) {
                defect[3 * i + j] = residual[i] * f.normal[j] / dn;
                if (cut(k)) {
                    const double consistent =
                        f.hi < 0 ? gauss[f.lo][3 * i + j]
                                 : (gauss[f.lo][3 * i + j] + gauss[f.hi][3 * i + j]) / 2;
                    defect[3 * i + j] += mean[3 * i + j] - consistent;
                }
            }
        value += f.area * dn * density(defect, stiffness);
    }
    return value;
}
void assembled_face_gradient_work(const ElasticMesh& m, bool free) {
    auto p = nonaffine_problem(m);
    if (free) {
        p.support = ElasticProblem::Support::Rigid321;
        p.prescribed.clear();
    }
    auto u = values(m.centroid, patch_a, patch_b);
    std::vector<V> v;
    for (std::size_t c = 0; c < u.size(); ++c) {
        const double q = static_cast<double>(c + 1);
        u[c] = add(u[c], {0.0002 * std::sin(q), 0.0003 * std::cos(q), -0.0001 * q});
        v.push_back({0.0004 * std::cos(q), -0.0001 * q, 0.0002 * std::sin(q)});
    }
    auto plus = u, minus = u;
    for (std::size_t c = 0; c < u.size(); ++c) {
        plus[c] = add(u[c], v[c]);
        minus[c] = sub(u[c], v[c]);
    }
    const double expected =
        (face_gradient_energy(m, p, plus) - face_gradient_energy(m, p, minus)) / 2;
    const auto a = assemble(m, p);
    double matrix_work = 0, traction_work = 0;
    for (const auto& b : a.blocks)
        matrix_work += dot(v[b.row], mul(b.value, u[b.column]));
    for (std::size_t c = 0; c < u.size(); ++c)
        matrix_work -= dot(v[c], a.rhs[c]);
    const auto force = tractions(a, u);
    for (std::size_t k = 0; k < m.faces.size(); ++k) {
        const auto& f = m.faces[k];
        traction_work += dot(f.hi < 0 ? scale(v[f.lo], -1) : sub(v[f.hi], v[f.lo]), force[k]);
    }
    near(matrix_work, expected, 1e-8, 1e-11, "matrix virtual work uses full cut face gradient");
    near(traction_work, expected, 1e-8, 1e-11, "assembled traction uses full cut face gradient");
}
void check_gradient(const ElasticMesh& m,
                    const ElasticProblem& p,
                    const std::vector<V>& u,
                    const M3& expected) {
    const auto g = detail::evaluate_cut_gradients(m, p, u, MPI_COMM_SELF);
    require(static_cast<bool>(g), g ? "" : "cut gradient evaluation: " + g.error().reason);
    require(g.value().size() == m.volume.size(), "gradient preserves caller cell order");
    for (std::size_t c = 0; c < m.volume.size(); ++c)
        if (m.is_cut[c])
            gradient_near(
                g.value()[c], expected, 1e-10, "cut gradient gid=" + std::to_string(m.gid[c]));
}
ElasticResult affine_patch(const ElasticMesh& m,
                           const M3& a = patch_a,
                           Material mat = laminate(0.3),
                           MPI_Comm comm = MPI_COMM_SELF) {
    auto p = problem(m, mat, 0);
    prescribe(p, m, a, patch_b);
    const auto r = solve(m, p, comm);
    field_near(r.U_cell, values(m.centroid, a, patch_b), 0, 1e-10, "affine cut cell patch");
    field_near(r.U_face, values(face_centers(m), a, patch_b), 0, 1e-10, "affine cut face patch");
    check_gradient(m, p, r.U_cell, a);
    diagnostics(m, r, comm);
    physical_residual(m, p, r, 1e-10);
    return r;
}
ElasticResult free_expansion(const ElasticMesh& m,
                             double tolerance,
                             const std::string& pc = "jacobi",
                             MPI_Comm comm = MPI_COMM_SELF) {
    auto p = problem(m);
    p.preconditioner = pc;
    const auto r = solve(m, p, comm);
    const double strain = 80e-5;
    const M3 a{strain, 0, 0, 0, strain, 0, 0, 0, strain};
    const V b = scale(center(m), -strain);
    field_near(r.U_cell,
               values(m.centroid, a, b),
               tolerance * length(m) * strain,
               0,
               "free expansion after Rigid321 gauge");
    field_near(r.U_face,
               values(face_centers(m), a, b),
               tolerance * length(m) * strain,
               0,
               "free faces use owner extrapolation");
    const auto force = tractions(assemble(m, p), r.U_cell);
    double stiffness = 0;
    for (const auto& c : p.C)
        for (double v : c)
            stiffness = std::max(stiffness, std::abs(v));
    for (std::size_t f = 0; f < force.size(); ++f)
        require(norm(force[f]) / (stiffness * strain * m.faces[f].area) <= tolerance,
                "normalized integrated free traction, face=" + std::to_string(f));
    diagnostics(m, r, comm);
    physical_residual(m, p, r, tolerance);
    return r;
}
std::string reject_before_ksp(const ElasticMesh& m,
                              const ElasticProblem& p,
                              const std::string& code = "INTERNAL") {
    PetscLogEvent event = 0;
    require(PetscLogEventGetId("KSPSolve", &event) == PETSC_SUCCESS, "find KSP event");
    PetscEventPerfInfo before{}, after{};
    require(PetscLogEventGetPerfInfo(PETSC_DETERMINE, event, &before) == PETSC_SUCCESS,
            "KSP before");
    const auto r = solve_elastic(m, p, MPI_COMM_SELF);
    require(PetscLogEventGetPerfInfo(PETSC_DETERMINE, event, &after) == PETSC_SUCCESS, "KSP after");
    require(after.count == before.count, "geometry/metadata failure must precede KSPSolve");
    require(!r, "invalid cut geometry/metadata must return error");
    const auto why = r.error().reason;
    require(why.find(code) != std::string::npos, "expected " + code + ", got: " + why);
    const auto assembly = detail::assemble_elastic(m, p, MPI_COMM_SELF);
    require(!assembly && assembly.error().reason.find(code) != std::string::npos,
            "production and diagnostic assembly reject with the same error contract");
    return why;
}
void error_location(const std::string& why, std::int64_t gid, std::size_t face) {
    require(std::regex_search(why, std::regex("gid[ =:]+" + std::to_string(gid) + "\\b")),
            "error identifies lo gid: " + why);
    require(
        std::regex_search(why, std::regex("face(?:_index)?[ =:]+" + std::to_string(face) + "\\b")),
        "error identifies rank-local faces index: " + why);
}
double error_rho(const std::string& why) {
    std::smatch match;
    require(std::regex_search(why, match, std::regex("(?:rho|ρ)[ =:]+([-+0-9.eE]+)")),
            "error contains numeric rho: " + why);
    return std::stod(match[1].str());
}
M3 compatible_slanted_a() {
    // For isotropic nu=0.3: sigma is uniaxial along t, so sigma*n=0.
    const V n = scale(V{1, 0.37, 0.29}, 1 / norm(V{1, 0.37, 0.29}));
    const V t = scale(cross(n, {0, 0, 1}), 1 / norm(cross(n, {0, 0, 1})));
    const V v = cross(n, t);
    M3 a{0, -0.0007, 0.0004, 0.0007, 0, -0.0002, -0.0004, 0.0002, 0};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            a[3 * i + j] += 0.0013 * (t[i] * t[j] - 0.3 * (n[i] * n[j] + v[i] * v[j]));
    return a;
}
ElasticMesh shear_cube(double rho) {
    auto m = grid(1, 1, 1);
    // Center the highly sheared cell before transforming: subtracting large
    // translated coordinates otherwise loses O(epsilon/rho) in the moments.
    // Round the tangent upward so sqrt(rho^2+tangent^2) is exactly one in
    // double precision, including the exact gate-equality case.
    const double tangent = std::nextafter(std::sqrt(1 - rho * rho), 1.0);
    const V origin = m.centroid[0];
    const M3 t{2 * rho, 0, 0, 2 * tangent, 2, 0, 0, 0, 2};
    m.centroid[0] = {};

    m.volume[0] = 8 * rho;
    m.is_cut = {1};
    m.gid = {61};
    for (auto& f : m.faces) {
        f.centroid = mul(t, sub(f.centroid, origin));
        const V n{f.normal[0] / (2 * rho) - tangent * f.normal[1] / (2 * rho),
                  f.normal[1] / 2,
                  f.normal[2] / 2};
        f.area *= 8 * rho * norm(n);
        f.normal = scale(n, 1 / norm(n));
    }
    m.z_top = 1;
    return m;
}
ElasticMesh tied_minima(bool near_tie, int ranks) {
    auto m = grid(2, 1, 1, ranks);
    m.gid = {9007199254740999LL, 9007199254740993LL};
    for (auto& f : m.faces)
        if (f.side == 0 || f.side == 1)
            f.centroid[1] += 0.1 + (near_tie && f.lo == 0 ? 1e-14 : 0);
    return m;
}
void zero_diagnostics(const ElasticMesh& m, MPI_Comm comm) {
    auto p = problem(m, isotropic(), 0);
    prescribe(p, m, M3{});
    const auto r = solve(m, p, comm);
    require(max_norm(r.U_cell) <= 1e-12 * length(m), "zero field with displaced face centers");
    diagnostics(m, r, comm);
}
void matrix_compatibility(const ElasticMesh& m) {
    const auto a = assemble(m, problem(m));
    const std::size_t dofs = 3 * m.volume.size();
    std::vector<double> dense(dofs * dofs);
    for (const auto& b : a.blocks)
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                dense[(3 * b.row + i) * dofs + 3 * b.column + j] += b.value[3 * i + j];
    double norm2 = 0, symmetry2 = 0;
    for (std::size_t i = 0; i < dofs; ++i)
        for (std::size_t j = 0; j < dofs; ++j) {
            norm2 += dense[i * dofs + j] * dense[i * dofs + j];
            const double delta = dense[i * dofs + j] - dense[j * dofs + i];
            symmetry2 += delta * delta;
        }
    require(norm2 > 0, "nonzero cut stiffness");
    std::cout << "实测 matrix_symmetry_defect=" << std::sqrt(symmetry2 / norm2) << '\n';
    require(std::sqrt(symmetry2 / norm2) <= 1e-13, "cut stiffness symmetry");
    for (int mode = 0; mode < 6; ++mode) {
        V axis{};
        axis[mode % 3] = 1;
        std::vector<V> q;
        for (const auto& x : m.centroid)
            q.push_back(mode < 3 ? axis : cross(axis, sub(x, center(m))));
        double q2 = 0, right2 = 0, left2 = 0, load = 0, rhs2 = 0;
        for (std::size_t i = 0; i < dofs; ++i) {
            double right = 0, left = 0;
            for (std::size_t j = 0; j < dofs; ++j) {
                right += dense[i * dofs + j] * q[j / 3][j % 3];
                left += dense[j * dofs + i] * q[j / 3][j % 3];
            }
            right2 += right * right;
            left2 += left * left;
            q2 += q[i / 3][i % 3] * q[i / 3][i % 3];
            load += q[i / 3][i % 3] * a.rhs[i / 3][i % 3];
            rhs2 += a.rhs[i / 3][i % 3] * a.rhs[i / 3][i % 3];
        }
        require(std::sqrt(right2 / (q2 * norm2)) <= 1e-12, "cut right rigid mode");
        require(std::sqrt(left2 / (q2 * norm2)) <= 1e-12, "cut transpose rigid mode");
        require(std::abs(load) <= 1e-12 * std::sqrt(q2 * rhs2), "cut left-mode load compatibility");
    }
    const auto force = tractions(a, values(m.centroid, patch_a, patch_b));
    std::vector<V> by_face(m.volume.size()), by_matrix(m.volume.size());
    for (std::size_t c = 0; c < m.volume.size(); ++c)
        by_matrix[c] = scale(a.rhs[c], -1);
    const auto u = values(m.centroid, patch_a, patch_b);
    for (const auto& b : a.blocks)
        by_matrix[b.row] = add(by_matrix[b.row], mul(b.value, u[b.column]));
    for (std::size_t f = 0; f < m.faces.size(); ++f) {
        by_face[m.faces[f].lo] = sub(by_face[m.faces[f].lo], force[f]);
        if (m.faces[f].hi >= 0)
            by_face[m.faces[f].hi] = add(by_face[m.faces[f].hi], force[f]);
    }
    field_near(by_face, by_matrix, 0, 1e-12, "one internal traction enters opposite equations");
}
void embedded_output(double z_shift, double nz) {
    auto m = grid(2, 2, 1);
    // Rigid rotation about y retains exact moments and gives top normal (s,0,nz).
    const double s = std::sqrt(1 - nz * nz);
    const M3 rotation{nz, 0, s, 0, 1, 0, -s, 0, nz};
    for (auto& x : m.centroid)
        x = mul(rotation, x);
    for (std::size_t f = 0; f < m.faces.size(); ++f) {
        auto& face = m.faces[f];
        face.centroid = mul(rotation, face.centroid);
        face.normal = mul(rotation, face.normal);
        if (face.side == 5) {
            face.side = -1;
            m.is_embedded[f] = 1;
        }
    }
    m.is_cut.assign(m.volume.size(), 1);
    m.z_top = -std::numeric_limits<double>::infinity();
    for (std::size_t f = 0; f < m.faces.size(); ++f)
        if (m.is_embedded[f])
            m.z_top = std::max(m.z_top, m.faces[f].centroid[2]);
    m.z_top += z_shift;
    const auto r = free_expansion(m, 1e-10);
    std::vector<V> expected;
    for (std::size_t f = 0; f < m.faces.size(); ++f) {
        const auto& face = m.faces[f];
        if (m.is_embedded[f]) {
            require(!r.face_prescribed[f], "embedded face remains free");
            if (face.normal[2] > 0.99 && face.centroid[2] >= m.z_top - 1e-13)
                expected.push_back({face.centroid[0], face.centroid[1], r.U_face[f][2]});
        }
    }
    field_near(r.top_samples, expected, 0, 1e-14, "embedded top thresholds and caller face order");
    if (nz <= 0.99 || z_shift > 1e-13)
        require(expected.empty(), "threshold excludes top samples");
    else
        require(!expected.empty(), "qualifying embedded top samples exist");
}
void mpi_case(Suite& suite, ElasticMesh m, bool empty_owner, const std::string& pc) {
    if (empty_owner)
        std::fill(m.rank.begin(), m.rank.end(), 0);
    auto p = problem(m);
    p.preconditioner = pc;
    const auto distributed = solve(m, p, suite.comm);
    auto serial_mesh = m;
    std::fill(serial_mesh.rank.begin(), serial_mesh.rank.end(), 0);
    const auto serial = solve(serial_mesh, p, MPI_COMM_SELF);
    // All collectives finish before rank-local comparisons.
    diagnostics(m, distributed, suite.comm);
    field_near(distributed.U_cell, serial.U_cell, 0, 1e-8, "np1/np2 cut cells in caller gid order");
    field_near(distributed.U_face, serial.U_face, 0, 1e-8, "np1/np2 cut faces");
    require(distributed.min_dn_ratio == serial.min_dn_ratio &&
                distributed.min_dn_gid == serial.min_dn_gid,
            "minimum rho/gid independent of partition");
    near(distributed.max_identity_defect,
         serial.max_identity_defect,
         2e-15,
         0,
         "global identity maximum independent of partition");
}
void nonaffine_solver(const ElasticMesh& m, bool free, MPI_Comm comm) {
    auto p = nonaffine_problem(m);
    if (free) {
        p.support = ElasticProblem::Support::Rigid321;
        p.prescribed.clear();
    }
    p.preconditioner = "jacobi";
    const auto jacobi = solve(m, p, comm);
    p.preconditioner = "gamg";
    const auto gamg = solve(m, p, comm);
    auto serial_mesh = m;
    std::fill(serial_mesh.rank.begin(), serial_mesh.rank.end(), 0);
    const auto serial = solve(serial_mesh, p, MPI_COMM_SELF);
    field_near(jacobi.U_cell, gamg.U_cell, 0, 1e-8, "nonaffine Jacobi/GAMG cells");
    field_near(jacobi.U_face, gamg.U_face, 0, 1e-8, "nonaffine Jacobi/GAMG faces");
    field_near(gamg.U_cell, serial.U_cell, 0, 1e-8, "nonaffine np1/np2 cells");
    field_near(gamg.U_face, serial.U_face, 0, 1e-8, "nonaffine np1/np2 faces");
    require(jacobi.iterations > 0 && gamg.iterations > 0, "nonaffine load exercises Krylov solve");
    physical_residual(serial_mesh, p, jacobi, 1e-10);
    physical_residual(serial_mesh, p, gamg, 1e-10);
}
} // namespace

void concave_acceptance(Suite& suite, int ranks, bool mpi_only) {
    if (!mpi_only)
        for (bool free : {false, true})
            suite.run(std::string("uncut nonaffine virtual work") + (free ? " free" : " Fixed"),
                      [=] {
                          auto m = grid(2, 2, 2);
                          m.is_cut.assign(m.volume.size(), 0);
                          assembled_face_gradient_work(m, free);
                      });
    for (bool slanted : {false, true})
        for (bool free : {false, true}) {
            const auto name = std::string(slanted ? "slanted" : "L") + (free ? " free" : " Fixed");
            if (!mpi_only)
                suite.run(name + " nonaffine face-gradient matrix and traction virtual work", [=] {
                    assembled_face_gradient_work(slanted ? slanted_cut() : concave_l(), free);
                });
            for (bool empty : {false, true}) {
                if (empty && !mpi_only)
                    continue;
                suite.run(name + " nonaffine Jacobi/GAMG" + (empty ? " empty owner" : ""), [&] {
                    auto m = slanted ? slanted_cut(ranks) : concave_l(ranks);
                    if (empty)
                        std::fill(m.rank.begin(), m.rank.end(), 0);
                    nonaffine_solver(m, free, suite.comm);
                });
            }
        }
    if (!mpi_only) {
        suite.run("L fixture seven cells, segmented interfaces and exact geometry", [] {
            const auto m = concave_l();
            require(m.volume.size() == 7, "seven cells");
            require(m.gid == std::vector<std::int64_t>{0, 1, 2, 3, 4, 5, 6}, "brief gids");
            near(m.volume[0], 3, 0, 0, "L volume");
            require(norm(sub(m.centroid[0], {5.0 / 6, 5.0 / 6, 0.5})) <= 1e-15, "L centroid");
            const std::vector<V> centers{{1.5, 1.5, 0.5},
                                         {2.5, 0.5, 0.5},
                                         {2.5, 1.5, 0.5},
                                         {0.5, 2.5, 0.5},
                                         {1.5, 2.5, 0.5},
                                         {2.5, 2.5, 0.5}};
            for (std::size_t c = 1; c < 7; ++c) {
                near(m.volume[c], 1, 0, 0, "unit cube volume");
                require(norm(sub(m.centroid[c], centers[c - 1])) <= 1e-15, "gid-specific centroid");
            }
            int twice = 0, top = 0, bottom = 0, l_internal = 0;
            std::vector<V> shared;
            for (std::size_t i = 0; i < m.faces.size(); ++i) {
                const auto& f = m.faces[i];
                require(!m.is_embedded[i], "L patch contains no embedded interfaces");
                if (f.lo == 0 && f.hi == 1) {
                    ++twice;
                    shared.push_back(f.centroid);
                }
                if (f.lo == 0 && f.hi >= 0)
                    ++l_internal;
                if (f.lo == 0 && f.side == 5)
                    ++top;
                if (f.lo == 0 && f.side == 4)
                    ++bottom;
            }
            require(twice == 2 && l_internal == 4 && top == 3 && bottom == 3,
                    "all L face segments retained");
            require(norm(sub(shared[0], shared[1])) > 0.1, "two distinct L/gid1 faces");
            fixture_geometry(m);
            require(geometry_reference(m).min_rho > 0, "L minimum rho positive");
        });
        suite.run("thin-arm fixture exact geometry and negative analytic rho", [] {
            const auto m = thin_arm_l();
            fixture_geometry(m);
            const auto d = sub(m.faces[0].centroid, m.centroid[0]);
            near(dot(d, m.faces[0].normal) / norm(d),
                 -11 / std::sqrt(746.0),
                 1e-15,
                 0,
                 "analytic notch rho");
        });
        suite.run("slanted fixture exact polygon and tetrahedral moments", [] {
            const auto m = slanted_cut();
            fixture_geometry(m);
            require(m.volume.size() > 8 && m.volume.size() < 32, "4x4x2 plane clipping");
            require(std::count(m.is_cut.begin(), m.is_cut.end(), 1) > 4, "multiple cut cells");
            require(geometry_reference(m).min_rho > 1e-6, "slanted fixture satisfies rho gate");
        });
        suite.run("L laminate f=0.3 general nonsymmetric affine patch",
                  [] { affine_patch(concave_l()); });
        suite.run("cut internal total d dot n permits negative owner face projection", [] {
            auto m = voxel_union({0, 0.2, 1}, {0, 0.2, 1}, {0, 0, 0, 1});
            m.is_cut[0] = 1;
            fixture_geometry(m);
            bool outside = false;
            for (const auto& f : m.faces)
                if (f.hi >= 0 && dot(sub(f.centroid, m.centroid[f.lo]), f.normal) < 0) {
                    outside = true;
                    require(dot(sub(m.centroid[f.hi], m.centroid[f.lo]), f.normal) > 0,
                            "total projected distance remains positive");
                }
            require(outside, "concave owner centroid lies beyond internal face");
            affine_patch(m);
        });
        suite.run("cut face values use inverse Euclidean distances and Green-Gauss sum", [] {
            const auto m = concave_l();
            auto p = problem(m, laminate(0.3), 0);
            prescribe(p, m, patch_a, patch_b);
            p.prescribed.front().U[1] +=
                0.0005; // Nonaffine data distinguishes interpolation weights.
            const auto r = solve(m, p, MPI_COMM_SELF);
            const auto g = detail::evaluate_cut_gradients(m, p, r.U_cell, MPI_COMM_SELF);
            require(static_cast<bool>(g), g ? "" : g.error().reason);
            require(g.value().size() == m.volume.size(), "one production gradient per cell");
            std::vector<M3> gauss(m.volume.size());
            bool unequal_distances = false;
            for (std::size_t f = 0; f < m.faces.size(); ++f) {
                const auto& face = m.faces[f];
                if (face.hi >= 0 && (m.is_cut[face.lo] || m.is_cut[face.hi])) {
                    const auto lo_delta = sub(face.centroid, m.centroid[face.lo]);
                    const auto hi_delta = sub(face.centroid, m.centroid[face.hi]);
                    const auto lo_value = add(r.U_cell[face.lo], mul(g.value()[face.lo], lo_delta));
                    const auto hi_value = add(r.U_cell[face.hi], mul(g.value()[face.hi], hi_delta));
                    const double wl = 1 / norm(lo_delta), wh = 1 / norm(hi_delta);
                    unequal_distances = unequal_distances || std::abs(wl - wh) > 0.1;
                    const auto expected =
                        scale(add(scale(lo_value, wl), scale(hi_value, wh)), 1 / (wl + wh));
                    require(norm(sub(r.U_face[f], expected)) <= 1e-12 * max_norm(r.U_face),
                            "inverse Euclidean distance weighted extrapolations");
                }
                for (int side = 0; side < (face.hi < 0 ? 1 : 2); ++side) {
                    const auto c = side == 0 ? face.lo : face.hi;
                    const auto area = scale(face.normal, side == 0 ? face.area : -face.area);
                    for (int i = 0; i < 3; ++i)
                        for (int j = 0; j < 3; ++j)
                            gauss[c][3 * i + j] += r.U_face[f][i] * area[j] / m.volume[c];
                }
            }
            require(unequal_distances, "fixture discriminates distance weights");
            gradient_near(
                g.value()[0], gauss[0], 1e-12, "production cut gradient equals signed face sum");
            physical_residual(m, p, r, 1e-10);
        });
        suite.run("L exact-field Green-Gauss gradient", [] {
            const auto m = concave_l();
            auto p = problem(m, laminate(0.3), 0);
            prescribe(p, m, patch_a, patch_b);
            check_gradient(m, p, values(m.centroid, patch_a, patch_b), patch_a);
        });
        suite.run("L free expansion and every normalized face traction <=1e-10",
                  [] { free_expansion(concave_l(), 1e-10); });
        suite.run("L assembly symmetry, both rigid nullspaces and conservative traction",
                  [] { matrix_compatibility(concave_l()); });
        suite.run("thin-arm INTERNAL lo gid/face/rho before KSPSolve", [] {
            const auto m = thin_arm_l();
            const auto why = reject_before_ksp(m, problem(m));
            error_location(why, 73, 0);
            near(error_rho(why), -11 / std::sqrt(746.0), 1e-14, 0, "reported negative rho");
        });
        suite.run("embedded status alone classifies a cut face for rho gate", [] {
            auto m = thin_arm_l();
            m.is_cut.clear();
            const auto why = reject_before_ksp(m, problem(m));
            error_location(why, 73, 0);
            near(error_rho(why), -11 / std::sqrt(746.0), 1e-14, 0, "embedded-only negative rho");
        });
        suite.run("two disconnected cut entities independently reproduce affine fields", [] {
            const auto m = disconnected_cut();
            auto p = problem(m, laminate(0.3), 0);
            const M3 other{
                -0.0003, 0.0014, -0.0008, 0.0007, 0.0021, 0.0009, 0.0012, -0.0005, 0.0013};
            const V other_b{-0.0002, 0.0006, -0.0011};
            p.support = ElasticProblem::Support::Fixed;
            for (std::size_t f = 0; f < m.faces.size(); ++f) {
                const bool first = m.faces[f].lo == 0;
                p.prescribed.push_back({static_cast<std::int64_t>(f),
                                        add(mul(first ? patch_a : other, m.faces[f].centroid),
                                            first ? patch_b : other_b)});
            }
            const auto r = solve(m, p, MPI_COMM_SELF);
            const auto g = detail::evaluate_cut_gradients(m, p, r.U_cell, MPI_COMM_SELF);
            require(static_cast<bool>(g), g ? "" : g.error().reason);
            require(g.value().size() == 2, "two independent gradients");
            for (std::size_t c = 0; c < 2; ++c) {
                const auto& a = c == 0 ? patch_a : other;
                const auto exact = add(mul(a, m.centroid[c]), c == 0 ? patch_b : other_b);
                require(norm(sub(r.U_cell[c], exact)) <= 1e-10 * norm(exact),
                        "per-entity relative displacement");
                gradient_near(g.value()[c], a, 1e-10, "per-entity gradient");
            }
        });
        suite.run("slanted nonzero-strain affine patch with compatible free surface", [] {
            const auto m = slanted_cut();
            const auto a = compatible_slanted_a();
            const auto p = problem(m, isotropic(), 0);
            for (std::size_t f = 0; f < m.faces.size(); ++f)
                if (m.is_embedded[f])
                    require(norm(stress_traction(p, a, m.faces[f])) <=
                                1e-14 * 25e9 * matrix_norm(a) * m.faces[f].area,
                            "manufactured affine stress has zero embedded traction");
            affine_patch(m, a, isotropic());
        });
        suite.run("slanted free rigid rotation preserves cut gradients", [] {
            const auto m = slanted_cut();
            const M3 rotation{0, -0.0007, 0.0004, 0.0007, 0, -0.0002, -0.0004, 0.0002, 0};
            check_gradient(
                m, problem(m, isotropic(), 0), values(m.centroid, rotation, {}), rotation);
            matrix_compatibility(m);
        });
        suite.run("slanted free expansion and embedded traction <=1e-9",
                  [] { free_expansion(slanted_cut(), 1e-9); });
        suite.run("cut cell with one prescribed face, three globally noncollinear constraints", [] {
            auto m = concave_l();
            auto p = problem(m, isotropic(), 0);
            p.support = ElasticProblem::Support::Fixed;
            // One bottom segment of L, one top of gid1 and x=3 of gid6.
            for (std::size_t f = 0; f < m.faces.size(); ++f) {
                const auto& face = m.faces[f];
                const bool wanted = (face.lo == 0 && face.side == 4 && p.prescribed.empty()) ||
                                    (face.lo == 1 && face.side == 5) ||
                                    (face.lo == 6 && face.side == 1);
                if (wanted)
                    p.prescribed.push_back({static_cast<std::int64_t>(f), {}});
            }
            require(p.prescribed.size() == 3, "three prescribed faces");
            const auto a = m.faces[p.prescribed[0].face].centroid;
            const auto b = m.faces[p.prescribed[1].face].centroid;
            const auto c = m.faces[p.prescribed[2].face].centroid;
            require(norm(cross(sub(b, a), sub(c, a))) > 0.1, "noncollinear prescribed centers");
            const auto r = solve(m, p, MPI_COMM_SELF);
            require(max_norm(r.U_cell) <= 1e-12 * length(m), "partial prescription zero field");
        });
        for (double rho : {0.999e-6, 1e-6, 1.001e-6})
            suite.run("rho gate edge " + std::to_string(rho * 1e6) + "e-6", [=] {
                const auto m = shear_cube(rho);
                auto p = problem(m, isotropic(), 0);
                prescribe(p, m, M3{});
                if (rho <= 1e-6) {
                    const auto why = reject_before_ksp(m, p);
                    error_location(why, 61, 0);
                    near(error_rho(why), rho, 2e-15, 0, "threshold rho reported");
                } else {
                    const auto r = solve(m, p, MPI_COMM_SELF);
                    diagnostics(m, r, MPI_COMM_SELF);
                }
            });
        suite.run("rho gate does not apply to uncut faces", [] {
            auto m = shear_cube(0.999e-6);
            m.is_cut.clear();
            zero_diagnostics(m, MPI_COMM_SELF);
        });
        suite.run("broken cut moment identity rejected before KSP", [] {
            auto m = concave_l();
            m.volume[0] *= 1.001;
            const auto why = reject_before_ksp(m, problem(m));
            require(std::regex_search(why, std::regex("gid[ =:]+0\\b")), "identity error gid");
            require(why.find("defect") != std::string::npos, "identity error includes defect");
        });
        suite.run("broken area-vector closure despite valid moment identity", [] {
            auto m = grid(1, 1, 1);
            m.is_cut = {1};
            m.faces[0].area *= 1.1;
            m.faces[0].centroid =
                add(m.centroid[0], scale(sub(m.faces[0].centroid, m.centroid[0]), 1 / 1.1));
            const auto reference = geometry_reference(m);
            require(reference.moment_defect[0] <= 1e-14, "moment identity alone still holds");
            require(reference.closure_defect[0] > 0.09, "constant field would not reproduce");
            const auto why = reject_before_ksp(m, problem(m));
            require(why.find("closure") != std::string::npos, "error identifies area closure");
        });
        const std::vector<std::pair<std::string, std::function<void(ElasticMesh&)>>> malformed{
            {"short is_cut", [](auto& m) { m.is_cut.pop_back(); }},
            {"long is_cut", [](auto& m) { m.is_cut.push_back(0); }},
            {"negative is_cut", [](auto& m) { m.is_cut[0] = -1; }},
            {"nonbinary is_cut", [](auto& m) { m.is_cut[0] = 2; }}};
        for (const auto& [name, mutate] : malformed)
            suite.run(name + " rejected before KSP", [&] {
                auto m = concave_l();
                mutate(m);
                reject_before_ksp(m, problem(m), "INPUT_INVALID");
            });
        for (const auto& name : {"volume", "area", "boundary distance", "internal distance"})
            for (double bad : {0.0,
                               -1.0,
                               std::numeric_limits<double>::infinity(),
                               std::numeric_limits<double>::quiet_NaN()}) {
                if ((std::string(name).find("distance") != std::string::npos) && bad < 0)
                    continue;
                const std::string suffix =
                    std::isnan(bad) ? " NaN" : (std::isinf(bad) ? " Inf" : std::to_string(bad));
                suite.run(std::string("INTERNAL cut ") + name + suffix, [=] {
                    auto m = concave_l();
                    std::size_t face = 0;
                    if (std::string(name) == "volume")
                        m.volume[0] = bad;
                    else if (std::string(name) == "area")
                        m.faces[0].area = bad;
                    else if (std::string(name) == "boundary distance")
                        m.faces[0].centroid = bad == 0 ? m.centroid[0] : V{bad, 0, 0};
                    else {
                        while (m.faces[face].hi < 0)
                            ++face;
                        m.centroid[m.faces[face].hi] =
                            bad == 0 ? m.centroid[m.faces[face].lo] : V{bad, 0, 0};
                    }
                    const auto why = reject_before_ksp(m, problem(m));
                    require(std::regex_search(why, std::regex("gid[ =:]+0\\b")),
                            "geometry error includes gid");
                    if (std::string(name).find("distance") != std::string::npos)
                        error_location(why, m.gid[m.faces[face].lo], face);
                });
            }
        suite.run("internal cut face gradient preserves general affine field", [] {
            const auto m = concave_l();
            const auto u = values(m.centroid, patch_a, patch_b);
            const std::vector<M3> g(m.volume.size(), patch_a);
            for (std::size_t f = 0; f < m.faces.size(); ++f) {
                const auto& face = m.faces[f];
                if (face.hi >= 0 && (m.is_cut[face.lo] || m.is_cut[face.hi])) {
                    const auto result = detail::evaluate_cut_face_gradient(m, f, u, g, {});
                    require(static_cast<bool>(result), result ? "" : result.error().reason);
                    gradient_near(result.value(), patch_a, 1e-13, "affine face gradient");
                }
            }
        });
        suite.run("cut internal face gradient averages both distinct cell gradients", [] {
            const auto m = concave_l();
            const auto u = values(m.centroid, patch_a, patch_b);
            std::vector<M3> g(m.volume.size(), patch_a);
            g[0] = {0.001, -0.002, 0.003, -0.004, 0.002, 0.001, 0.002, -0.003, 0.001};
            for (std::size_t f = 0; f < m.faces.size(); ++f) {
                const auto& face = m.faces[f];
                if (face.hi < 0 || face.lo != 0)
                    continue;
                M3 expected{};
                for (int k = 0; k < 9; ++k)
                    expected[k] = (g[face.lo][k] + g[face.hi][k]) / 2;
                const auto d = sub(m.centroid[face.hi], m.centroid[face.lo]);
                const auto correction = sub(sub(u[face.hi], u[face.lo]), mul(expected, d));
                for (int i = 0; i < 3; ++i)
                    for (int j = 0; j < 3; ++j)
                        expected[3 * i + j] += correction[i] * face.normal[j] / dot(d, face.normal);
                const auto result = detail::evaluate_cut_face_gradient(m, f, u, g, {});
                require(static_cast<bool>(result), result ? "" : result.error().reason);
                gradient_near(result.value(), expected, 1e-13, "arithmetic mean cell gradients");
            }
        });
        suite.run("cut face correction uses n/(d dot n), including boundary", [] {
            const auto m = concave_l();
            const auto u = values(m.centroid, patch_a, patch_b);
            const std::vector<M3> g(m.volume.size(), M3{});
            for (std::size_t f = 0; f < m.faces.size(); ++f) {
                const auto& face = m.faces[f];
                if (!m.is_cut[face.lo] && (face.hi < 0 || !m.is_cut[face.hi]))
                    continue;
                const V ub = add(mul(patch_a, face.centroid), patch_b);
                const auto d =
                    sub(face.hi < 0 ? face.centroid : m.centroid[face.hi], m.centroid[face.lo]);
                const auto du = sub(face.hi < 0 ? ub : u[face.hi], u[face.lo]);
                M3 expected{};
                for (int i = 0; i < 3; ++i)
                    for (int j = 0; j < 3; ++j)
                        expected[3 * i + j] = du[i] * face.normal[j] / dot(d, face.normal);
                const auto result = detail::evaluate_cut_face_gradient(m, f, u, g, ub);
                require(static_cast<bool>(result), result ? "" : result.error().reason);
                gradient_near(result.value(), expected, 1e-13, "nonorthogonal compact correction");
            }
        });
        for (const auto& [label, shift, nz] : std::vector<std::tuple<std::string, double, double>>{
                 {"horizontal", 0, 1},
                 {"z within tolerance", 0.5e-13, 1},
                 {"z outside tolerance", 2e-13, 1},
                 {"normal equal threshold", 0, 0.99},
                 {"normal above threshold", 0, 0.99001}})
            suite.run("embedded output " + label, [=] { embedded_output(shift, nz); });
        for (bool slanted : {false, true})
            suite.run(
                std::string(slanted ? "slanted" : "L") + " Jacobi/GAMG relative agreement", [=] {
                    const auto m = slanted ? slanted_cut() : concave_l();
                    const auto a = free_expansion(m, slanted ? 1e-9 : 1e-10, "jacobi");
                    const auto b = free_expansion(m, slanted ? 1e-9 : 1e-10, "gamg");
                    field_near(a.U_cell, b.U_cell, 0, 1e-8, "cut preconditioner independence");
                });
    }
    for (bool near_tie : {false, true})
        suite.run(near_tie ? "strict unequal rho near-tie, large gids"
                           : "exact rho tie, large gids",
                  [&] {
                      const auto m = tied_minima(near_tie, ranks);
                      const auto g = geometry_reference(m);
                      require(g.min_gid == (near_tie ? m.gid[0] : m.gid[1]),
                              "independent tie fixture expectation");
                      zero_diagnostics(m, suite.comm);
                  });
    if (mpi_only)
        suite.run("exact rho tie with empty-owner rank", [&] {
            auto m = tied_minima(false, ranks);
            std::fill(m.rank.begin(), m.rank.end(), 0);
            zero_diagnostics(m, suite.comm);
        });
    if (mpi_only)
        for (bool slanted : {false, true})
            for (bool empty : {false, true})
                for (const std::string pc : {"jacobi", "gamg"})
                    suite.run(std::string("np2 ") + (slanted ? "slanted " : "L ") + pc +
                                  (empty ? " empty owner" : " split owners"),
                              [&] {
                                  mpi_case(suite,
                                           slanted ? slanted_cut(ranks) : concave_l(ranks),
                                           empty,
                                           pc);
                              });
}
} // namespace elastic_test

#ifndef CHIPCAE_CONCAVE_NO_MAIN
int main(int argc, char** argv) {
    std::cout << std::unitbuf;
    if (PetscInitialize(&argc, &argv, nullptr, nullptr) != PETSC_SUCCESS)
        return 2;
    KSPInitializePackage();
    PetscLogDefaultBegin();
    elastic_test::Suite suite(MPI_COMM_WORLD);
    int ranks = 1;
    MPI_Comm_size(suite.comm, &ranks);
    elastic_test::concave_acceptance(suite, ranks);
    const int result = suite.finish();
    PetscFinalize();
    return result;
}
#endif
