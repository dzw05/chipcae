#include "elastic_test_mesh.hpp"

#include <iomanip>

using namespace elastic_test;
namespace {
struct Ply {
    double lo, hi;
    Material mat;
};
struct CLT {
    double strain, curvature;
};
// Independent through-thickness integration: epsilon_yy=0, sigma_zz=0.
// Resultant axial force and bending moment both vanish. No solver stencil is used.
CLT clt(const std::vector<Ply>& plies, double dT) {
    double A = 0, B = 0, D = 0, N = 0, M = 0;
    for (const auto& ply : plies) {
        const auto& c = ply.mat.C;
        double tx = 0, tz = 0;
        for (int j = 0; j < 3; ++j) {
            tx += c[j] * ply.mat.alpha[j] * dT;
            tz += c[12 + j] * ply.mat.alpha[j] * dT;
        }
        const double q = c[0] - c[2] * c[12] / c[14];
        const double t = tx - c[2] * tz / c[14];
        const double dz = ply.hi - ply.lo;
        const double z2 = (ply.hi * ply.hi - ply.lo * ply.lo) / 2;
        const double z3 = (ply.hi * ply.hi * ply.hi - ply.lo * ply.lo * ply.lo) / 3;
        A += q * dz;
        B += q * z2;
        D += q * z3;
        N += t * dz;
        M += t * z2;
    }
    const double det = A * D - B * B;
    require(det > 0, "positive laminate ABD determinant");
    return {(D * N - B * M) / det, (A * M - B * N) / det};
}
double
timoshenko(double E1, double h1, double alpha1, double E2, double h2, double alpha2, double dT) {
    const double m = h1 / h2, n = E1 / E2, h = h1 + h2;
    return 6 * (alpha2 - alpha1) * dT * (1 + m) * (1 + m) /
           (h * (3 * (1 + m) * (1 + m) + (1 + m * n) * (m * m + 1 / (m * n))));
}
double fit_curvature(const std::vector<V>& samples, double length) {
    // Centered/scaled quadratic with a linear y term removes rigid translation
    // and tilt; fit the central half of the strip to exclude end effects.
    std::vector<std::array<double, 4>> rows;
    std::vector<double> values;
    for (const auto& s : samples) {
        const double x = (s[0] - length / 2) / length;
        if (std::abs(x) <= 0.25) {
            rows.push_back({1, x, x * x, s[1] / length});
            values.push_back(s[2]);
        }
    }
    require(rows.size() >= 12, "quadratic fit needs at least twelve central top-face samples");
    // Modified Gram-Schmidt QR, dropping the y column only if all samples are collinear.
    std::array<std::vector<double>, 4> q;
    std::array<double, 16> r{};
    std::array<double, 4> rhs{};
    int active = 4;
    for (int j = 0; j < 4; ++j) {
        auto& v = q[j];
        for (const auto& row : rows)
            v.push_back(row[j]);
        for (int k = 0; k < j; ++k) {
            r[4 * k + j] = std::inner_product(q[k].begin(), q[k].end(), v.begin(), 0.0);
            for (std::size_t i = 0; i < v.size(); ++i)
                v[i] -= r[4 * k + j] * q[k][i];
        }
        const double n = std::sqrt(std::inner_product(v.begin(), v.end(), v.begin(), 0.0));
        if (j == 3 && n <= 1e-13) {
            active = 3;
            break;
        }
        require(n > 1e-13, "full rank central quadratic curvature fit");
        r[4 * j + j] = n;
        for (auto& a : v)
            a /= n;
        rhs[j] = std::inner_product(v.begin(), v.end(), values.begin(), 0.0);
    }
    std::array<double, 4> c{};
    for (int i = active - 1; i >= 0; --i) {
        double v = rhs[i];
        for (int j = i + 1; j < active; ++j)
            v -= r[4 * i + j] * c[j];
        c[i] = v / r[4 * i + i];
    }
    // epsilon_xx=e0+kappa*z -> U_z=-kappa*x^2/2. Keep the sign.
    return -2 * c[2] / (length * length);
}
ElasticMesh strip(int refinement, double L = 0.08, double W = 0.002) {
    const int factor = 1 << refinement;
    auto m = grid(axis(32 * factor, L), axis(2 * factor, W), axis(8 * factor, 0.002));
    for (std::size_t c = 0; c < m.layer.size(); ++c)
        m.layer[c] = m.centroid[c][2] < 0.001 ? 0 : 1;
    return m;
}
ElasticProblem layered(const ElasticMesh& m, const std::vector<Ply>& plies, double dT) {
    auto p = problem(m, plies.front().mat, dT);
    for (std::size_t c = 0; c < m.volume.size(); ++c) {
        const auto& mat = plies[static_cast<std::size_t>(m.layer[c])].mat;
        p.C[c] = mat.C;
        p.alpha[c] = mat.alpha;
    }
    return p;
}
V clt_displacement(const V& x, double L, const std::vector<Ply>& plies, double dT, CLT ref) {
    const double xc = x[0] - L / 2;
    double uz = -ref.curvature * xc * xc / 2;
    for (const auto& ply : plies) {
        const double hi = std::min(x[2], ply.hi);
        if (hi <= ply.lo)
            continue;
        const auto& c = ply.mat.C;
        double tz = 0;
        for (int j = 0; j < 3; ++j)
            tz += c[12 + j] * ply.mat.alpha[j] * dT;
        uz += ((tz - c[12] * ref.strain) * (hi - ply.lo) -
               c[12] * ref.curvature * (hi * hi - ply.lo * ply.lo) / 2) /
              c[14];
    }
    return {(ref.strain + ref.curvature * x[2]) * xc, 0, uz};
}
void reference_sanity() {
    // Equal moduli/thickness give kappa=3*(alpha2-alpha1)*dT/(2*h).
    near(timoshenko(25e9, 0.001, 1e-5, 25e9, 0.001, 2e-5, 80),
         0.6,
         1e-14,
         0,
         "hand-derived equal-layer curvature");
    const std::vector<Ply> plies{{0, 0.001, isotropic(25e9, 0, 1e-5)},
                                 {0.001, 0.002, isotropic(110e9, 0, 17e-6)}};
    const double t = timoshenko(25e9, 0.001, 1e-5, 110e9, 0.001, 17e-6, 80);
    near(clt(plies, 80).curvature, t, 1e-14, 1e-13, "independent ABD versus Timoshenko");
    near(clt(plies, -80).curvature, -t, 1e-14, 1e-13, "cooling reverses signed curvature");
    std::vector<V> samples;
    for (int i = 0; i < 20; ++i)
        for (int j = 0; j < 3; ++j) {
            const double x = (i + 0.5) / 20, y = j * 0.1;
            samples.push_back({x, y, 3 + 0.2 * x + 0.4 * y - 0.6 * x * x / 2});
        }
    near(fit_curvature(samples, 1), 0.6, 1e-11, 0, "signed quadratic fitter removes plane");
}
void timoshenko_refinements(double dT) {
    // nu=0 separates beam curvature from finite-width Poisson/anticlastic effects.
    // Both plies are isotropic synthetic materials; no copper mixing is used.
    const std::vector<Ply> plies{{0, 0.001, isotropic(25e9, 0, 1e-5)},
                                 {0.001, 0.002, isotropic(110e9, 0, 17e-6)}};
    const double reference = timoshenko(25e9, 0.001, 1e-5, 110e9, 0.001, 17e-6, dT);
    std::array<double, 3> error{};
    bool solved = true;
    std::string missing;
    // Run every refinement even during RED: a missing coarse solve must not hide
    // an unexercised fine acceptance case.
    for (int level = 0; level < 3; ++level) {
        const auto m = strip(level);
        const auto p = layered(m, plies, dT);
        auto result = solve_elastic(m, p, MPI_COMM_SELF);
        if (!result) {
            solved = false;
            missing += " level=" + std::to_string(level) + ": " + result.error().reason;
            continue;
        }
        const auto& r = result.value();
        check_output(m, p, r);
        const double k = fit_curvature(r.top_samples, 0.08);
        require(k * reference > 0, "Timoshenko curvature sign");
        error[level] = std::abs(k - reference) / std::abs(reference);
        std::cout << std::setprecision(17) << "实测 Timoshenko dT=" << dT << " level=" << level
                  << " cells=" << m.volume.size() << " signed_kappa=" << k
                  << " reference=" << reference << " relative_error=" << error[level] << '\n';
    }
    require(solved, "all three Timoshenko refinements must solve:" + missing);
    require(error[2] <= 0.02, "finest signed Timoshenko curvature error <=2%");
    for (int i = 0; i < 2; ++i) {
        // An exact patch is acceptable; do not take log(0).
        const double order = error[i + 1] <= 1e-12 ? std::numeric_limits<double>::infinity()
                                                   : std::log2(error[i] / error[i + 1]);
        std::cout << "实测 Timoshenko observed_order[" << i << "]=" << order << '\n';
        require(order >= 1, "observed Timoshenko order >=1 on both refinement pairs");
    }
}
void orthotropic_plane_strain() {
    const double dT = 80, L = 0.08;
    const std::vector<Ply> plies{{0, 0.001, isotropic()}, {0.001, 0.002, laminate(0.3)}};
    const auto ref = clt(plies, dT);
    require(std::abs(ref.curvature) > 0.01, "nontrivial orthotropic CLT curvature");
    const auto m = strip(1, L, 0.002);
    auto p = layered(m, plies, dT);
    p.support = ElasticProblem::Support::Fixed;
    // Full-vector prescribed faces are the public API. Lateral y faces impose
    // epsilon_yy=0 and the independent CLT bending kinematics; x ends and z faces
    // remain traction-free. This specifies the plane-strain convention explicitly.
    for (std::size_t f = 0; f < m.faces.size(); ++f)
        if (m.faces[f].side == 2 || m.faces[f].side == 3)
            p.prescribed.push_back({static_cast<std::int64_t>(f),
                                    clt_displacement(m.faces[f].centroid, L, plies, dT, ref)});
    const auto r = solve(m, p, MPI_COMM_SELF);
    check_output(m, p, r);
    const double curvature = fit_curvature(r.top_samples, L);
    const double error = std::abs(curvature - ref.curvature) / std::abs(ref.curvature);
    std::cout << std::setprecision(17) << "实测 plane-strain CLT signed_kappa=" << curvature
              << " reference=" << ref.curvature << " relative_error=" << error << '\n';
    require(curvature * ref.curvature > 0, "orthotropic CLT curvature sign");
    require(error <= 0.03, "orthotropic f=0.3 laminate signed curvature error <=3%");
}
} // namespace
int main(int argc, char** argv) {
    std::cout << std::unitbuf;
    if (PetscInitialize(&argc, &argv, nullptr, nullptr) != PETSC_SUCCESS)
        return 2;
    Suite s(MPI_COMM_WORLD);
    s.run("independent curvature reference and signed fit", reference_sanity);
    s.run("free Timoshenko heating three refinements", [] { timoshenko_refinements(80); });
    s.run("free Timoshenko cooling three refinements", [] { timoshenko_refinements(-80); });
    s.run("orthotropic copper laminate plane-strain CLT", orthotropic_plane_strain);
    const int result = s.finish();
    PetscFinalize();
    return result;
}
