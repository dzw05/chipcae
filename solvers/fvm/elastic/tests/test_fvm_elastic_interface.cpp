#include "elastic_stencil.hpp"
#include "elastic_test_mesh.hpp"

#include <chrono>
#include <cstring>
#include <iomanip>
#include <optional>
#include <petscksp.h>
#include <regex>

using namespace elastic_test;
namespace {
constexpr double e0 = 1e-4, temperature = 50, nu = 0.3;
constexpr double field_tolerance = 1e-10;

PetscErrorCode matrix_diagnostics(
    PetscLogEvent event, int, PetscObject object, PetscObject, PetscObject, PetscObject) {
    PetscLogEvent solve_event = 0;
    PetscCall(PetscLogEventGetId("KSPSolve", &solve_event));
    if (event != solve_event)
        return PETSC_SUCCESS;
    const auto ksp = reinterpret_cast<KSP>(object);
    Mat a = nullptr;
    MatInfo info{};
    PetscInt iterations = 0;
    PetscReal residual = 0;
    PetscCall(KSPGetOperators(ksp, &a, nullptr));
    PetscCall(MatGetInfo(a, MAT_GLOBAL_SUM, &info));
    PetscCall(KSPGetIterationNumber(ksp, &iterations));
    PetscCall(KSPGetResidualNorm(ksp, &residual));
    PetscCall(PetscPrintf(PetscObjectComm(object),
                          "PETSc nz_used=%.0f KSP iterations=%" PetscInt_FMT " residual=%.17g\n",
                          static_cast<double>(info.nz_used),
                          iterations,
                          static_cast<double>(residual)));
    return PETSC_SUCCESS;
}

struct Patch {
    ElasticMesh mesh;
    ElasticProblem problem;
    double interface_z = 0;
    std::array<double, 2> ez{};

    V exact(const V& x) const {
        return {e0 * x[0],
                e0 * x[1],
                ez[0] * std::min(x[2], interface_z) + ez[1] * std::max(0.0, x[2] - interface_z)};
    }
    M3 gradient(std::size_t c) const { return {e0, 0, 0, 0, e0, 0, 0, 0, ez[mesh.layer[c]]}; }
    std::vector<V> exact_cells() const {
        std::vector<V> out;
        for (const auto& x : mesh.centroid)
            out.push_back(exact(x));
        return out;
    }
    std::vector<std::size_t> interfaces() const {
        std::vector<std::size_t> out;
        for (std::size_t k = 0; k < mesh.faces.size(); ++k) {
            const auto& f = mesh.faces[k];
            if (f.hi >= 0 && mesh.layer[f.lo] != mesh.layer[f.hi])
                out.push_back(k);
        }
        require(!out.empty(), "fixture contains material interface faces");
        return out;
    }
};

Patch patch(ElasticMesh mesh, double interface_z, bool e_jump = true, bool alpha_jump = true) {
    Patch out{std::move(mesh), {}, interface_z, {}};
    out.problem = problem(out.mesh, isotropic(), temperature);
    for (int layer = 0; layer < 2; ++layer) {
        const double E = layer == 1 && e_jump ? 100e9 : 25e9;
        const double alpha = layer == 1 && alpha_jump ? 3e-5 : 1e-5;
        const double mu = E / (2 * (1 + nu));
        const double lambda = E * nu / ((1 + nu) * (1 - 2 * nu));
        // Independent plane-stress reference: sigma_zz = sigma_xz = sigma_yz = 0.
        out.ez[layer] =
            ((3 * lambda + 2 * mu) * alpha * temperature - 2 * lambda * e0) / (lambda + 2 * mu);
        const auto mat = isotropic(E, nu, alpha);
        for (std::size_t c = 0; c < out.mesh.volume.size(); ++c)
            if ((out.mesh.centroid[c][2] > interface_z ? 1 : 0) == layer) {
                out.mesh.layer[c] = layer;
                out.problem.C[c] = mat.C;
                out.problem.alpha[c] = mat.alpha;
            }
    }
    out.problem.support = ElasticProblem::Support::Fixed;
    for (std::size_t k = 0; k < out.mesh.faces.size(); ++k) {
        const auto& f = out.mesh.faces[k];
        if (f.hi < 0 && f.side >= 0 && f.side < 4)
            out.problem.prescribed.push_back({static_cast<std::int64_t>(k), out.exact(f.centroid)});
    }
    return out;
}

Patch patch_grid(bool graded, bool e_jump = true, bool alpha_jump = true) {
    const auto z = axis(graded ? 4 : 5, 1, graded ? 1.3 : 1);
    return patch(grid(axis(graded ? 6 : 4, 1, graded ? 1.3 : 1),
                      axis(graded ? 5 : 4, 1, graded ? 1.3 : 1),
                      z),
                 z[2],
                 e_jump,
                 alpha_jump);
}

std::vector<V> interface_values(const Patch& p, const ElasticResult& r) {
    require(r.U_face.size() == p.mesh.faces.size(), "one production value per input face");
    std::vector<V> out;
    for (auto k : p.interfaces())
        out.push_back(r.U_face[k]);
    return out;
}
std::vector<V> exact_interfaces(const Patch& p) {
    std::vector<V> out;
    for (auto k : p.interfaces())
        out.push_back(p.exact(p.mesh.faces[k].centroid));
    return out;
}
void relative_field(const std::vector<V>& got, const std::vector<V>& want, const std::string& why) {
    require(got.size() == want.size() && !want.empty(), why + " nonempty matching arrays");
    double error = 0;
    for (std::size_t i = 0; i < got.size(); ++i)
        error = std::max(error, norm(sub(got[i], want[i])));
    require(max_norm(want) > 0, why + " nonzero reference scale");
    std::cout << why << " normalized_max_error=" << error / max_norm(want) << '\n';
    field_near(got, want, 0, field_tolerance, why);
}

detail::ElasticAssembly assembly(const ElasticMesh& m, const ElasticProblem& p) {
    auto a = detail::assemble_elastic(m, p, MPI_COMM_SELF);
    require(static_cast<bool>(a), a ? "" : "assembly: " + a.error().reason);
    return std::move(a.value());
}
std::vector<M3> production_gradients(const Patch& p, const ElasticResult& r) {
    // This existing private entry point evaluates reconstruction() for uncut cells too.
    // These are the actual LS maps used by assembly and returned face reconstruction.
    auto g = detail::evaluate_cut_gradients(p.mesh, p.problem, r.U_cell, MPI_COMM_SELF);
    require(static_cast<bool>(g), g ? "" : "production gradients: " + g.error().reason);
    require(g.value().size() == p.mesh.volume.size(), "one production gradient per cell");
    return std::move(g.value());
}

M3 stress(const ElasticProblem& p, std::size_t c, const M3& g) {
    const std::array<double, 6> strain{g[0] - p.alpha[c][0] * p.dT[c],
                                       g[4] - p.alpha[c][1] * p.dT[c],
                                       g[8] - p.alpha[c][2] * p.dT[c],
                                       g[5] + g[7],
                                       g[2] + g[6],
                                       g[1] + g[3]};
    std::array<double, 6> s{};
    for (int i = 0; i < 6; ++i)
        for (int j = 0; j < 6; ++j)
            s[i] += p.C[c][6 * i + j] * strain[j];
    return {s[0], s[5], s[4], s[5], s[1], s[3], s[4], s[3], s[2]};
}
double matrix_norm(const M3& m) {
    return std::sqrt(std::inner_product(m.begin(), m.end(), m.begin(), 0.0));
}

void check_gradients(const Patch& p, const ElasticResult& r) {
    const auto g = production_gradients(p, r);
    double error = 0, reference = 0;
    for (std::size_t c = 0; c < g.size(); ++c) {
        const auto exact = p.gradient(c);
        M3 delta{};
        for (int j = 0; j < 9; ++j)
            delta[j] = g[c][j] - exact[j];
        error = std::max(error, matrix_norm(delta));
        reference = std::max(reference, matrix_norm(exact));
    }
    std::cout << "T4 production gradient normalized_max_error=" << error / reference << '\n';
    require(std::isfinite(error) && error <= field_tolerance * reference,
            "T4 one-sided production gradients reproduce piecewise-affine derivatives");
}

void check_traction(const Patch& p, const ElasticResult& r) {
    const auto g = production_gradients(p, r);
    double jump = 0, sigma_scale = 0;
    for (auto k : p.interfaces()) {
        const auto& f = p.mesh.faces[k];
        std::array<V, 2> traction{};
        for (int side = 0; side < 2; ++side) {
            const auto c = static_cast<std::size_t>(side == 0 ? f.lo : f.hi);
            const auto delta = sub(f.centroid, p.mesh.centroid[c]);
            const double signed_d = dot(delta, f.normal);
            require(side == 0 ? signed_d > 0 : signed_d < 0, "positive half-face distances");
            const auto tangent = sub(delta, scale(f.normal, signed_d));
            // The brief's two signed normal derivatives, using the actual returned U_f.
            const auto dn =
                scale(sub(sub(r.U_face[k], r.U_cell[c]), mul(g[c], tangent)), 1 / signed_d);
            const auto old_dn = mul(g[c], f.normal);
            auto one_sided = g[c];
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j)
                    one_sided[3 * i + j] += (dn[i] - old_dn[i]) * f.normal[j];
            const auto sigma = stress(p.problem, c, one_sided);
            require(std::isfinite(matrix_norm(sigma)), "finite one-sided engineering stress");
            sigma_scale = std::max(sigma_scale, matrix_norm(sigma));
            traction[side] = mul(sigma, f.normal); // Same lo-to-hi normal on both sides.
        }
        jump = std::max(jump, norm(sub(traction[0], traction[1])));
    }
    require(sigma_scale > 0, "nonzero patch stress scale");
    std::cout << "T4 independent traction normalized_max_jump=" << jump / sigma_scale << '\n';
    require(std::isfinite(jump) && jump <= field_tolerance * sigma_scale,
            "T4 one-sided constitutive tractions are continuous");
}

void run_patch(Suite& suite, const std::string& name, const Patch& p) {
    std::optional<ElasticResult> result;
    suite.run(name + " solve", [&] {
        for (std::size_t c = 0; c < p.mesh.volume.size(); ++c) {
            const auto sigma = stress(p.problem, c, p.gradient(c));
            require(norm(mul(sigma, {0, 0, 1})) <= 1e-14 * matrix_norm(sigma),
                    "analytical fixture has zero traction along interface and free top/bottom");
        }
        const auto begin = std::chrono::steady_clock::now();
        result = solve(p.mesh, p.problem, MPI_COMM_SELF);
        std::cout << name << " wall_s="
                  << std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count()
                  << " iterations=" << result->iterations << " residual=" << result->residual
                  << '\n';
    });
    auto solved = [&]() -> const ElasticResult& {
        require(result.has_value(), "patch solve must succeed before checking fields");
        return *result;
    };
    suite.run("T1 " + name + " cell field",
              [&] { relative_field(solved().U_cell, p.exact_cells(), "T1 " + name + " U_cell"); });
    suite.run("T1 " + name + " interface field", [&] {
        relative_field(
            interface_values(p, solved()), exact_interfaces(p), "T1 " + name + " U_face");
    });
    suite.run("T4 " + name + " production gradients", [&] { check_gradients(p, solved()); });
    suite.run("T4 " + name + " constitutive tractions", [&] { check_traction(p, solved()); });
}

void matrix_invariants(Patch p) {
    p.problem.support = ElasticProblem::Support::Rigid321;
    p.problem.prescribed.clear();
    const auto a = assembly(p.mesh, p.problem);
    std::map<std::pair<std::int64_t, std::int64_t>, M3> blocks;
    double norm2 = 0;
    for (const auto& b : a.blocks) {
        require(blocks.emplace(std::make_pair(b.row, b.column), b.value).second,
                "unique ordered assembly block keys");
        for (double v : b.value)
            norm2 += v * v;
    }
    require(std::isfinite(norm2) && norm2 > 0, "finite nonzero matrix norm");
    double asymmetry2 = 0;
    for (const auto& [key, b] : blocks) {
        const auto it = blocks.find({key.second, key.first});
        require(it != blocks.end(), "symmetric block support");
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) {
                const double d = b[3 * i + j] - it->second[3 * j + i];
                asymmetry2 += d * d;
            }
    }
    const double symmetry = std::sqrt(asymmetry2 / norm2);
    std::cout << "T2 cells=" << p.mesh.volume.size() << " blocks=" << blocks.size()
              << " scalar_block_entries=" << 9 * blocks.size() << " symmetry=" << symmetry << '\n';
    require(symmetry <= 1e-13, "T2 ||A-A^T||F/||A||F <= 1e-13");
    const auto origin = center(p.mesh);
    for (int mode = 0; mode < 6; ++mode) {
        V axis{};
        axis[mode % 3] = 1;
        std::vector<V> r(p.mesh.volume.size()), ar(r.size());
        double rnorm2 = 0, arnorm2 = 0;
        for (std::size_t c = 0; c < r.size(); ++c) {
            r[c] = mode < 3 ? axis : cross(axis, sub(p.mesh.centroid[c], origin));
            rnorm2 += dot(r[c], r[c]);
        }
        for (const auto& b : a.blocks)
            ar[b.row] = add(ar[b.row], mul(b.value, r[b.column]));
        for (const auto& v : ar)
            arnorm2 += dot(v, v);
        const double residual = std::sqrt(arnorm2 / (norm2 * rnorm2));
        std::cout << "T2 rigid_mode=" << mode << " normalized_residual=" << residual << '\n';
        require(std::isfinite(residual) && residual <= 1e-12,
                "T2 ||Ar||2/(||A||F ||r||2) <= 1e-12, mode=" + std::to_string(mode));
    }
}

void identical_assembly(const detail::ElasticAssembly& a, const detail::ElasticAssembly& b) {
    require(a.blocks.size() == b.blocks.size(), "bitwise assembly block count");
    require(a.rhs.size() == b.rhs.size(), "bitwise assembly rhs count");
    std::pair<std::int64_t, std::int64_t> previous{-1, -1};
    for (std::size_t i = 0; i < a.blocks.size(); ++i) {
        const auto& x = a.blocks[i];
        const auto& y = b.blocks[i];
        const auto key = std::make_pair(x.row, x.column);
        require(previous < key, "strict row/column block order");
        previous = key;
        require(x.row == y.row && x.column == y.column, "identical ordered block keys");
        require(std::memcmp(x.value.data(), y.value.data(), sizeof(double) * 9) == 0,
                "identical block values (without struct padding)");
    }
    for (std::size_t c = 0; c < a.rhs.size(); ++c)
        require(std::memcmp(a.rhs[c].data(), b.rhs[c].data(), sizeof(double) * 3) == 0,
                "identical rhs vector bytes");
}
void reduction(bool graded, bool equal_eigenstrain) {
    auto p = patch_grid(graded, false, false);
    auto single = p.mesh;
    std::fill(single.layer.begin(), single.layer.end(), 0);
    auto equivalent = p.problem;
    if (equal_eigenstrain)
        for (std::size_t c = 0; c < p.mesh.volume.size(); ++c)
            if (p.mesh.layer[c] == 1) {
                equivalent.alpha[c] = scale(equivalent.alpha[c], 2);
                equivalent.dT[c] /= 2;
            }
    identical_assembly(assembly(p.mesh, equivalent), assembly(single, p.problem));
}

void rank_failure(MPI_Comm comm, int ranks) {
    auto p = patch(grid(1, 1, 2), 0.5);
    p.problem.support = ElasticProblem::Support::Rigid321;
    p.problem.prescribed.clear();
    // Free lateral traction rows determine strain but cannot identify the rotation
    // about z: all remaining displacement samples lie on the z axis. A local
    // interface solve must report this deficient rank rather than truncate it.
    for (std::size_t c = 0; c < p.mesh.rank.size(); ++c)
        p.mesh.rank[c] = ranks == 2 ? p.mesh.layer[c] : 0;
    const auto f = p.interfaces().front();
    const auto a = detail::assemble_elastic(p.mesh, p.problem, comm);
    int rejected = a ? 0 : 1, all_rejected = 0;
    MPI_Allreduce(&rejected, &all_rejected, 1, MPI_INT, MPI_MIN, comm);
    // No assertion precedes the collective, including on an empty owner rank.
    require(all_rejected != 0, "rank-deficient interface closure must return INTERNAL");
    const auto& reason = a.error().reason;
    require(reason.find("INTERNAL") != std::string::npos, "rank failure has INTERNAL code");
    require(std::regex_search(
                reason,
                std::regex("gid[ =:]+" + std::to_string(p.mesh.gid[p.mesh.faces[f].lo]) + "\\b")),
            "rank failure identifies lo gid: " + reason);
    require(std::regex_search(reason, std::regex("face[ =:]+" + std::to_string(f) + "\\b")),
            "rank failure identifies face: " + reason);
}

void cut_exclusion(bool cut_lo) {
    const auto z = axis(4, 1, 1.3);
    auto p = patch(grid(axis(2), axis(2), z), z[2]);
    p.mesh.is_cut.resize(p.mesh.volume.size());
    for (std::size_t c = 0; c < p.mesh.volume.size(); ++c)
        p.mesh.is_cut[c] = p.mesh.layer[c] == (cut_lo ? 0 : 1);
    const auto r = solve(p.mesh, p.problem, MPI_COMM_SELF);
    const auto g = production_gradients(p, r);
    std::vector<V> got, expected;
    bool unequal = false;
    for (auto k : p.interfaces()) {
        const auto& f = p.mesh.faces[k];
        const auto dl = sub(f.centroid, p.mesh.centroid[f.lo]);
        const auto dh = sub(f.centroid, p.mesh.centroid[f.hi]);
        const double wl = 1 / norm(dl), wh = 1 / norm(dh);
        unequal = unequal || std::abs(wl - wh) > 0.1;
        got.push_back(r.U_face[k]);
        expected.push_back(scale(add(scale(add(r.U_cell[f.lo], mul(g[f.lo], dl)), wl),
                                     scale(add(r.U_cell[f.hi], mul(g[f.hi], dh)), wh)),
                                 1 / (wl + wh)));
    }
    require(unequal, "cut exclusion discriminates unequal half-face distances");
    field_near(got, expected, 0, 1e-13, "cut material jumps retain legacy face extrapolation");
    identical_assembly(assembly(p.mesh, p.problem), assembly(p.mesh, p.problem));
}

void nonpositive_acoustic_pivot() {
    auto p = patch(grid(axis(2), axis(2), axis(4, 1e120)), 0.5e120);
    p.problem.support = ElasticProblem::Support::Rigid321;
    p.problem.prescribed.clear();
    std::fill(p.problem.dT.begin(), p.problem.dT.end(), 0);
    for (std::size_t c = 0; c < p.mesh.volume.size(); ++c)
        p.problem.C[c] = isotropic(p.mesh.layer[c] == 0 ? 1e-220 : 4e-220).C;
    const auto k = p.interfaces().front();
    const auto& f = p.mesh.faces[k];
    const double dl = dot(sub(f.centroid, p.mesh.centroid[f.lo]), f.normal);
    const double dh = dot(sub(p.mesh.centroid[f.hi], f.centroid), f.normal);
    // Finite SPD input, positive distances, but the normal acoustic pivot of
    // H = K_lo/d_lo + K_hi/d_hi underflows to zero. This exercises the specified
    // nonpositive-pivot error without adding a production fault-injection hook.
    const double pivot = p.problem.C[f.lo][14] / dl + p.problem.C[f.hi][14] / dh;
    require(!(pivot > 0), "fixture produces a nonpositive normal acoustic pivot");
    const auto a = detail::assemble_elastic(p.mesh, p.problem, MPI_COMM_SELF);
    require(!a, "nonpositive acoustic pivot must reject assembly");
    const auto& reason = a.error().reason;
    require(reason.find("INTERNAL") != std::string::npos,
            "nonpositive acoustic pivot reports INTERNAL: " + reason);
    require(std::regex_search(reason,
                              std::regex("gid[ =:]+" + std::to_string(p.mesh.gid[f.lo]) + "\\b")),
            "nonpositive acoustic pivot identifies lo gid: " + reason);
    require(std::regex_search(reason, std::regex("face[ =:]+" + std::to_string(k) + "\\b")),
            "nonpositive acoustic pivot identifies face: " + reason);
}

void multiple_interfaces() {
    auto m = grid(3, 3, 4);
    auto p = problem(m, isotropic(), 0);
    for (std::size_t c = 0; c < m.volume.size(); ++c) {
        const bool upper = m.centroid[c][2] > 0.5;
        const bool right = m.centroid[c][0] > 0.5;
        if (upper != right)
            p.C[c] = isotropic(100e9).C;
    }
    const M3 rotation{0, -0.0003, 0.0002, 0.0003, 0, -0.0001, -0.0002, 0.0001, 0};
    const V translation{0.0004, -0.0005, 0.0006};
    prescribe(p, m, rotation, translation);
    std::vector<int> counts(m.volume.size());
    for (const auto& f : m.faces)
        if (f.hi >= 0 && p.C[f.lo] != p.C[f.hi]) {
            ++counts[f.lo];
            ++counts[f.hi];
        }
    require(*std::max_element(counts.begin(), counts.end()) >= 2,
            "fixture has a cell incident to multiple interface faces");
    const auto r = solve(m, p, MPI_COMM_SELF);
    std::vector<V> cells, faces;
    for (const auto& x : m.centroid)
        cells.push_back(add(mul(rotation, x), translation));
    for (const auto& f : m.faces)
        faces.push_back(add(mul(rotation, f.centroid), translation));
    relative_field(r.U_cell, cells, "multiple-interface rigid U_cell");
    relative_field(r.U_face, faces, "multiple-interface rigid U_face");
}

void free_boundary_closure() {
    auto p = patch(grid(2, 1, 4), 0.5, true, false);
    p.problem.support = ElasticProblem::Support::Rigid321;
    p.problem.prescribed.clear();
    const auto r = solve(p.mesh, p.problem, MPI_COMM_SELF);
    std::vector<V> exact;
    const auto origin = center(p.mesh);
    for (const auto& x : p.mesh.centroid)
        exact.push_back(scale(sub(x, origin), 1e-5 * temperature));
    relative_field(r.U_cell, exact, "free boundary constitutive closure thermal expansion");
}

void skew_interface_diagnostic(bool exact) {
    auto mesh = patch_grid(true).mesh;
    constexpr double shear = 0.2;
    for (auto& x : mesh.centroid)
        x[0] += shear * x[2];
    for (auto& f : mesh.faces) {
        f.centroid[0] += shear * f.centroid[2];
        f.normal[2] -= shear * f.normal[0]; // inverse-transpose area vector
        const double scale = norm(f.normal);
        f.area *= scale;
        f.normal = elastic_test::scale(f.normal, 1 / scale);
    }
    const auto p = patch(std::move(mesh), axis(4, 1, 1.3)[2]);
    auto u = p.exact_cells();
    if (!exact)
        for (std::size_t c = 0; c < u.size(); ++c)
            for (int j = 0; j < 3; ++j)
                u[c][j] += 0.0001 * std::sin(static_cast<double>((c + 1) * (j + 2)));
    const auto got = detail::evaluate_elastic_interfaces(p.mesh, p.problem, u, MPI_COMM_SELF);
    require(static_cast<bool>(got), got ? "" : got.error().reason);
    require(got.value().size() == p.interfaces().size(), "diagnostic covers every marked face");
    double jump = 0, stress_scale = 0;
    std::vector<V> faces;
    for (const auto& sample : got.value()) {
        const auto& f = p.mesh.faces[sample.face];
        faces.push_back(sample.displacement);
        for (int side = 0; side < 2; ++side) {
            const auto c = static_cast<std::size_t>(side == 0 ? f.lo : f.hi);
            const auto sigma = stress(p.problem, c, sample.gradient[side]);
            stress_scale = std::max(stress_scale, matrix_norm(sigma));
            const auto independent = mul(sigma, f.normal);
            require(norm(sub(sample.traction[side], independent)) <= 1e-12 * matrix_norm(sigma),
                    "diagnostic agrees with independent engineering stress");
            if (exact) {
                const auto want = p.gradient(c);
                M3 error{};
                for (int j = 0; j < 9; ++j)
                    error[j] = sample.gradient[side][j] - want[j];
                require(matrix_norm(error) <= field_tolerance * matrix_norm(want),
                        "skew half-face gradients reproduce the piecewise-affine patch");
            }
        }
        jump = std::max(jump, norm(sub(sample.traction[0], sample.traction[1])));
    }
    require(stress_scale > 0 && jump <= field_tolerance * stress_scale,
            "skew unequal-distance one-sided tractions balance for arbitrary cell values");
    if (exact)
        relative_field(faces, exact_interfaces(p), "skew diagnostic U_face");
}

void mpi_comparison(Suite& suite, bool graded) {
    auto p = patch_grid(graded);
    const std::string name = std::string("T5 ") + (graded ? "graded" : "uniform");
    for (std::size_t c = 0; c < p.mesh.rank.size(); ++c)
        p.mesh.rank[c] = p.mesh.layer[c];
    std::optional<ElasticResult> parallel, serial;
    suite.run(name + " split-layer ownership solves", [&] {
        const auto full = solve_elastic(p.mesh, p.problem, suite.comm);
        auto serial_mesh = p.mesh;
        std::fill(serial_mesh.rank.begin(), serial_mesh.rank.end(), 0);
        const auto self = solve_elastic(serial_mesh, p.problem, MPI_COMM_SELF);
        // Finish the solves and all collectives before any assertion. A failing
        // rank must never strand a peer inside assembly, reconstruction or KSP.
        int success = full && self ? 1 : 0, all_success = 0;
        MPI_Allreduce(&success, &all_success, 1, MPI_INT, MPI_MIN, suite.comm);
        require(all_success != 0,
                !full ? "np2 solve: " + full.error().reason
                      : (!self ? "np1 solve: " + self.error().reason : "another rank failed"));
        parallel = full.value();
        serial = self.value();
        for (auto k : p.interfaces()) {
            const auto& f = p.mesh.faces[k];
            require(p.mesh.rank[f.lo] == 0 && p.mesh.rank[f.hi] == 1,
                    "every material interface face crosses ranks from lo owner 0 to hi owner 1");
        }
    });
    auto solved = [&] {
        require(parallel.has_value() && serial.has_value(), "both MPI patch solves succeed");
    };
    suite.run(name + " np2/np1 U_cell", [&] {
        solved();
        // Align cells explicitly by stable gid and faces by caller face index.
        std::map<std::int64_t, V> reference;
        for (std::size_t c = 0; c < p.mesh.gid.size(); ++c)
            reference.emplace(p.mesh.gid[c], serial->U_cell[c]);
        std::vector<V> want;
        for (auto gid : p.mesh.gid)
            want.push_back(reference.at(gid));
        relative_field(parallel->U_cell, want, name + " np2/np1 U_cell");
    });
    suite.run(name + " np2/np1 interface U_face", [&] {
        solved();
        relative_field(interface_values(p, *parallel),
                       interface_values(p, *serial),
                       name + " np2/np1 interface U_face");
    });
    suite.run(name + " np2 analytical U_cell", [&] {
        solved();
        relative_field(parallel->U_cell, p.exact_cells(), name + " np2 analytical U_cell");
    });
    suite.run(name + " np2 analytical interface U_face", [&] {
        solved();
        relative_field(interface_values(p, *parallel),
                       exact_interfaces(p),
                       name + " np2 analytical interface U_face");
    });
}
} // namespace

int main(int argc, char** argv) {
    std::cout << std::unitbuf << std::setprecision(17);
    if (PetscInitialize(&argc, &argv, nullptr, nullptr) != PETSC_SUCCESS)
        return 2;
    KSPInitializePackage();
    PetscLogDefaultBegin();
    PetscLogHandler metrics = nullptr;
    require(PetscLogHandlerCreateLegacy(
                MPI_COMM_WORLD, nullptr, matrix_diagnostics, nullptr, nullptr, &metrics) ==
                PETSC_SUCCESS,
            "create PETSc matrix diagnostics observer");
    require(PetscLogHandlerStart(metrics) == PETSC_SUCCESS,
            "start PETSc matrix diagnostics observer");
    Suite suite(MPI_COMM_WORLD);
    int ranks = 1;
    MPI_Comm_size(suite.comm, &ranks);
    char mode[32]{};
    PetscOptionsGetString(nullptr, nullptr, "-elastic_test_mode", mode, sizeof(mode), nullptr);
    if (std::string(mode) == "mpi") {
        suite.run("T5 np2 launcher", [&] { require(ranks == 2, "requires exactly two ranks"); });
        for (bool graded : {false, true})
            mpi_comparison(suite, graded);
        suite.run("rank failure is synchronized across interface owners",
                  [&] { rank_failure(suite.comm, ranks); });
    } else {
        for (bool graded : {false, true}) {
            const std::string name = graded ? "graded 6x5x(2+2)" : "uniform 4x4x(2+3)";
            run_patch(suite, name + " E+alpha jump", patch_grid(graded));
            run_patch(suite, name + " E-only jump", patch_grid(graded, true, false));
            run_patch(suite, name + " alpha-only jump", patch_grid(graded, false, true));
            suite.run("T2 " + name + " unconstrained operator",
                      [&] { matrix_invariants(patch_grid(graded)); });
            suite.run("T3 " + name + " identical material, distinct layer labels",
                      [&] { reduction(graded, false); });
            suite.run("equal eigenstrain with distinct alpha/dT is bitwise unchanged",
                      [&] { reduction(graded, true); });
            suite.run(name + " deterministic repeated assembly", [&] {
                const auto p = patch_grid(graded);
                identical_assembly(assembly(p.mesh, p.problem), assembly(p.mesh, p.problem));
            });
        }
        suite.run("T2 bimetal 8x4x(2+2) unconstrained operator", [&] {
            matrix_invariants(patch(grid(axis(8, 1), axis(4, 0.2), axis(4, 0.1)), 0.05));
        });
        run_patch(suite,
                  "interface meets free top/bottom and prescribed side corners",
                  patch(grid(2, 2, 2), 0.5));
        suite.run("interface cell with free lateral faces uses constitutive closure",
                  free_boundary_closure);
        suite.run("multiple interface faces preserve a shared rigid field", multiple_interfaces);
        for (bool exact : {false, true})
            suite.run(exact ? "skew interface piecewise-affine diagnostic"
                            : "skew interface arbitrary-field traction continuity",
                      [&] { skew_interface_diagnostic(exact); });
        suite.run("insufficient-rank interface closure reports INTERNAL gid and face",
                  [&] { rank_failure(MPI_COMM_SELF, 1); });
        suite.run("nonpositive acoustic Cholesky pivot reports INTERNAL gid and face",
                  nonpositive_acoustic_pivot);
        for (bool cut_lo : {false, true})
            suite.run(std::string("cut ") + (cut_lo ? "lo" : "hi") + " excludes interface path",
                      [&] { cut_exclusion(cut_lo); });
    }
    const int result = suite.finish();
    PetscLogHandlerStop(metrics);
    PetscLogHandlerDestroy(&metrics);
    PetscFinalize();
    return result;
}
