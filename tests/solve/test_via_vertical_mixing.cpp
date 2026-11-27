#include "vertical_mixing.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <jsoncons/json.hpp>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using chipcae::solve::Material;
using Json = jsoncons::json;
using Real = long double;
using Matrix = std::array<std::array<Real, 6>, 6>;

void require(bool condition, const std::string& message) {
    if (!condition)
        throw std::runtime_error(message);
}

void near(Real actual, Real expected, const std::string& field, Real tolerance) {
    const Real scale = std::max({std::abs(actual), std::abs(expected), Real{1e-30L}});
    if (!std::isfinite(actual) || !std::isfinite(expected) ||
        std::abs(actual - expected) > tolerance * scale) {
        std::ostringstream message;
        message << std::setprecision(18) << field << ": actual=" << actual
                << " expected=" << expected
                << " relative error=" << std::abs(actual - expected) / scale;
        throw std::runtime_error(message.str());
    }
}

// Independent normal-block adjugate and diagonal shear inversion. No MFEM or mixing helpers.
Matrix inverse(const Matrix& a) {
    Matrix result{};
    const Real a00 = a[0][0], a01 = a[0][1], a02 = a[0][2];
    const Real a11 = a[1][1], a12 = a[1][2], a22 = a[2][2];
    const Real c00 = a11 * a22 - a12 * a12;
    const Real c01 = a02 * a12 - a01 * a22;
    const Real c02 = a01 * a12 - a02 * a11;
    const Real det = a00 * c00 + a01 * c01 + a02 * c02;
    require(std::isfinite(det) && det > 0, "normal block must have positive determinant");
    result[0][0] = c00 / det;
    result[0][1] = result[1][0] = c01 / det;
    result[0][2] = result[2][0] = c02 / det;
    result[1][1] = (a00 * a22 - a02 * a02) / det;
    result[1][2] = result[2][1] = (a01 * a02 - a00 * a12) / det;
    result[2][2] = (a00 * a11 - a01 * a01) / det;
    for (int d = 3; d < 6; ++d) {
        require(std::isfinite(a[d][d]) && a[d][d] > 0, "positive engineering shear diagonal");
        result[d][d] = 1 / a[d][d];
    }
    return result;
}

struct Phase {
    Real E, nu, alpha, k, G, K, kappa;
    explicit Phase(const Material& a)
        : E(a.E), nu(a.nu), alpha(a.alpha), k(a.k), G(E / (2 * (1 + nu))),
          K(E / (2 * (1 + nu) * (1 - 2 * nu))), kappa(E / (3 * (1 - 2 * nu))) {}
};

// Literal closed forms 1--8 in the brief: Rayleigh/Maxwell Garnett, Hashin--Rosen,
// Hill/Hashin and Levin/Rosen--Hashin. K is plane-strain bulk; kappa is 3D bulk.
struct Expected {
    Matrix C{};
    std::array<Real, 3> alpha{}, k{};
    Real Ez, Et, nuzt, nutz, nut, Gzt, Gt, Kt;
};

Expected oracle(const Material& matrix, const Material& inclusion, Real f) {
    const Phase m(matrix), i(inclusion);
    Expected e{};
    const Real den = f / m.K + (1 - f) / i.K + 1 / m.G;
    const Real dn = i.nu - m.nu;
    e.Ez = f * i.E + (1 - f) * m.E + 4 * f * (1 - f) * dn * dn / den;
    e.nuzt = f * i.nu + (1 - f) * m.nu + f * (1 - f) * dn * (1 / m.K - 1 / i.K) / den;
    e.Gzt = m.G * (m.G * (1 - f) + i.G * (1 + f)) / (m.G * (1 + f) + i.G * (1 - f));
    e.Kt = i.K == m.K ? m.K : m.K + f / (1 / (i.K - m.K) + (1 - f) / (m.K + m.G));
    e.Gt = i.G == m.G
               ? m.G
               : m.G + f / (1 / (i.G - m.G) + (1 - f) * (m.K + 2 * m.G) / (2 * m.G * (m.K + m.G)));
    const Real factor = 1 + 4 * e.Kt * e.nuzt * e.nuzt / e.Ez;
    e.Et = 4 * e.Kt * e.Gt / (e.Kt + factor * e.Gt);
    e.nut = (e.Kt - factor * e.Gt) / (e.Kt + factor * e.Gt);
    e.nutz = e.nuzt * e.Et / e.Ez;
    Matrix S{};
    S[0][0] = S[1][1] = 1 / e.Et;
    S[2][2] = 1 / e.Ez;
    S[0][1] = S[1][0] = -e.nut / e.Et;
    S[0][2] = S[2][0] = S[1][2] = S[2][1] = -e.nuzt / e.Ez;
    S[3][3] = S[4][4] = 1 / e.Gzt; // yz, xz engineering shear
    S[5][5] = 1 / e.Gt;            // xy engineering shear
    e.C = inverse(S);
    const Matrix recovered = inverse(e.C);
    const Real amean = f * i.alpha + (1 - f) * m.alpha;
    const Real diff = 1 / i.kappa - 1 / m.kappa;
    const Real meanS = (f / i.kappa + (1 - f) / m.kappa) / 3;
    for (int d = 0; d < 3; ++d) {
        const Real row = recovered[d][0] + recovered[d][1] + recovered[d][2];
        e.alpha[d] = std::abs(diff) <= 1e-12L * std::max(1 / i.kappa, 1 / m.kappa)
                         ? amean
                         : amean + 3 * (i.alpha - m.alpha) / diff * (row - meanS);
    }
    e.k[0] = e.k[1] = m.k * (i.k + m.k + f * (i.k - m.k)) / (i.k + m.k - f * (i.k - m.k));
    e.k[2] = f * i.k + (1 - f) * m.k;
    return e;
}

Matrix stiffness(const chipcae::solve::Constitutive& value) {
    Matrix a{};
    for (int r = 0; r < 6; ++r)
        for (int c = 0; c < 6; ++c)
            a[r][c] = value.C(r, c);
    return a;
}

void bytes(double actual, double expected, const std::string& field) {
    static_assert(sizeof(double) == 8);
    require(std::memcmp(&actual, &expected, 8) == 0, field + ": endpoint bytes differ");
}

void endpoint(const Material& m, const Material& i, double f) {
    const auto actual = chipcae::solve::mixture_vertical(m, i, f);
    const auto legacy = chipcae::solve::mixture(m, i, f);
    double k[3]{}, old[3]{};
    chipcae::solve::copper_vertical_conductivity(m.k, i.k, f, k);
    chipcae::material::copper_laminate_conductivity(m.k, i.k, f, old);
    for (int r = 0; r < 6; ++r) {
        for (int c = 0; c < 6; ++c)
            bytes(actual.C(r, c), legacy.C(r, c), "C");
        if (r < 3) {
            bytes(actual.alpha[r], legacy.alpha[r], "alpha");
            bytes(k[r], old[r], "k");
        }
    }
}

// Compute both host choices independently of the oracle above (polarization forms).
std::array<Real, 4> hs(const Phase& host, const Phase& other, Real f) {
    const Real dk = other.k - host.k, dG = other.G - host.G, dK = other.K - host.K;
    const Real zeta = 2 * host.G * (host.K + host.G) / (host.K + 2 * host.G);
    return {host.k + 2 * host.k * f * dk / (2 * host.k + (1 - f) * dk),
            host.G + 2 * host.G * f * dG / (2 * host.G + (1 - f) * dG),
            host.K + (host.K + host.G) * f * dK / (host.K + host.G + (1 - f) * dK),
            host.G + zeta * f * dG / (zeta + (1 - f) * dG)};
}

void bounds(const Material& matrix,
            const Material& inclusion,
            Real f,
            const std::array<Real, 4>& actual,
            Real tol) {
    const Phase m(matrix), i(inclusion);
    const auto host = hs(m, i, f), swapped = hs(i, m, 1 - f);
    for (std::size_t d = 0; d < actual.size(); ++d) {
        const Real scale = std::max({std::abs(host[d]), std::abs(swapped[d]), Real{1e-30L}});
        require(actual[d] >= std::min(host[d], swapped[d]) - tol * scale &&
                    actual[d] <= std::max(host[d], swapped[d]) + tol * scale,
                "outside phase-swapped HS enclosure, component " + std::to_string(d));
        near(actual[d], host[d], "matrix-host HS side " + std::to_string(d), tol);
        if ((d == 0 && i.k >= m.k) || (d > 0 && i.K >= m.K && i.G >= m.G))
            require(host[d] <= swapped[d] + tol * scale, "hard inclusion must use lower HS side");
        if ((d == 0 && i.k <= m.k) || (d > 0 && i.K <= m.K && i.G <= m.G))
            require(host[d] >= swapped[d] - tol * scale, "soft inclusion must use upper HS side");
    }
}

void compare(const Material& m, const Material& i, double f, Real tol) {
    if (f == 0 || f == 1) {
        endpoint(m, i, f);
        return;
    }
    const auto expected = oracle(m, i, f);
    const auto actual = chipcae::solve::mixture_vertical(m, i, f);
    double k[3]{};
    chipcae::solve::copper_vertical_conductivity(m.k, i.k, f, k);
    for (int r = 0; r < 6; ++r) {
        for (int c = 0; c < 6; ++c)
            near(actual.C(r, c),
                 expected.C[r][c],
                 "C[" + std::to_string(r) + "][" + std::to_string(c) + "]",
                 tol);
        if (r < 3) {
            near(actual.alpha[r], expected.alpha[r], "alpha[" + std::to_string(r) + "]", tol);
            near(k[r], expected.k[r], "k[" + std::to_string(r) + "]", tol);
        }
    }
    const auto C = stiffness(actual), S = inverse(C);
    near(1 / S[2][2], expected.Ez, "Ez=1/Szz", tol);
    near(1 / S[0][0], expected.Et, "Eperp=1/Sxx", tol);
    near(1 / S[1][1], expected.Et, "Ey=1/Syy", tol);
    near(-S[0][2] / S[2][2], expected.nuzt, "nu_zt", tol);
    near(-S[0][2] / S[0][0], expected.nutz, "nu_tz", tol);
    near(-S[0][1] / S[0][0], expected.nut, "nu_perp", tol);
    near(1 / S[3][3], expected.Gzt, "Gyz", tol);
    near(1 / S[4][4], expected.Gzt, "Gxz", tol);
    near(1 / S[5][5], expected.Gt, "Gxy", tol);
    near((C[0][0] + C[0][1]) / 2, expected.Kt, "plane-strain Kperp", tol);
    bounds(m, i, f, {k[0], 1 / S[3][3], (C[0][0] + C[0][1]) / 2, 1 / S[5][5]}, tol);
}

// Keep conductivity independently runnable when the mechanical implementation is still absent.
void compare_conductivity(const Material& m, const Material& i, double f, Real tol) {
    double actual[3]{};
    chipcae::solve::copper_vertical_conductivity(m.k, i.k, f, actual);
    if (f == 0 || f == 1) {
        double legacy[3]{};
        chipcae::material::copper_laminate_conductivity(m.k, i.k, f, legacy);
        for (int d = 0; d < 3; ++d)
            bytes(actual[d], legacy[d], "conductivity");
        return;
    }
    const auto expected = oracle(m, i, f);
    for (int d = 0; d < 3; ++d)
        near(actual[d], expected.k[d], "conductivity[" + std::to_string(d) + "]", tol);
    const Phase a(m), b(i);
    const Real host = hs(a, b, f)[0], swapped = hs(b, a, 1 - f)[0];
    const Real slack = tol * std::max(std::abs(host), std::abs(swapped));
    require(actual[0] >= std::min(host, swapped) - slack &&
                actual[0] <= std::max(host, swapped) + slack,
            "conductivity HS enclosure");
    near(actual[0], host, "conductivity matrix-host HS side", tol);
    require(i.k >= m.k ? host <= swapped + slack : host >= swapped - slack,
            "conductivity host-bound direction");
}

void positive_definite(const Matrix& C, Real tol) {
    Matrix L{};
    for (int r = 0; r < 6; ++r) {
        for (int c = 0; c < 6; ++c) {
            require(std::isfinite(C[r][c]), "finite opening C");
            near(C[r][c], C[c][r], "symmetric opening C", tol);
        }
        for (int c = 0; c <= r; ++c) {
            Real pivot = C[r][c];
            for (int d = 0; d < c; ++d)
                pivot -= L[r][d] * L[c][d];
            if (r == c) {
                require(std::isfinite(pivot) && pivot > 0, "positive opening Cholesky pivot");
                L[r][c] = std::sqrt(pivot);
            } else {
                L[r][c] = pivot / L[c][c];
            }
        }
    }
}

template <class Function>
void invalid(Function&& function) {
    try {
        function();
    } catch (const chipcae::solve::InputError& error) {
        require(error.code == chipcae::ErrorCode::INPUT_INVALID,
                "InputError code must be INPUT_INVALID");
        return;
    } catch (const std::exception& error) {
        throw std::runtime_error(std::string("expected InputError, got: ") + error.what());
    }
    throw std::runtime_error("expected InputError(INPUT_INVALID), no exception thrown");
}

Material material(const Json& j) {
    Material m;
    m.id = j.at("id").as<std::string>();
    m.E = j.at("E").as<double>();
    m.nu = j.at("nu").as<double>();
    m.alpha = j.at("alpha").as<double>();
    m.k = j.at("k").as<double>();
    return m;
}

struct Cases {
    int count = 0, failures = 0;
    std::set<std::string> failure_kinds;
    template <class Function>
    void run(const std::string& name, Function&& function) {
        ++count;
        try {
            function();
        } catch (const std::exception& error) {
            ++failures;
            const std::string message = error.what();
            const bool new_kind = failure_kinds.insert(message.substr(0, message.find(':'))).second;
            if (failures <= 24 || new_kind)
                std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    }
};
} // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: test_via_vertical_mixing <repository fixture inputs.json>");
        std::ifstream input(argv[1]);
        require(input.good(), "fixture must be readable");
        const auto fixture = Json::parse(input);
        const auto m = material(fixture.at("materials").at("typical").at("matrix"));
        const auto i = material(fixture.at("materials").at("typical").at("copper"));
        const auto fractions = fixture.at("volume_fractions").as<std::vector<double>>();
        const Real tol = fixture.at("oracle_rel_tol").as<double>();
        const auto& robust = fixture.at("robustness");
        const auto nus = robust.at("nu_values").as<std::vector<double>>();
        Cases cases;
        std::cout << std::setprecision(17);
        auto sweep = [&](const std::string& name, const Material& a, const Material& b) {
            for (const double f : fractions) {
                cases.run(name + " f=" + std::to_string(f), [&] { compare(a, b, f, tol); });
                cases.run(name + " conductivity f=" + std::to_string(f),
                          [&] { compare_conductivity(a, b, f, tol); });
            }
        };
        sweep("typical", m, i);
        sweep("identical",
              material(fixture.at("materials").at("zero_contrast").at("matrix")),
              material(fixture.at("materials").at("zero_contrast").at("copper")));
        // Finite equal conductivities must not overflow in the MG numerator product.
        for (const double k :
             {std::numeric_limits<double>::max(), std::numeric_limits<double>::min()}) {
            for (const double f : fractions)
                cases.run("extreme equal conductivity k=" + std::to_string(k) +
                              " f=" + std::to_string(f),
                          [&] {
                              double actual[3]{};
                              chipcae::solve::copper_vertical_conductivity(k, k, f, actual);
                              for (int d = 0; d < 3; ++d)
                                  near(actual[d], k, "equal conductivity identity", tol);
                          });
        }
        for (double ratio : robust.at("k_ratio_kc_over_km").as<std::vector<double>>())
            for (double nm : nus)
                for (double ni : nus) {
                    auto a = m, b = i;
                    a.nu = nm;
                    b.nu = ni;
                    b.k = a.k * ratio;
                    sweep("hard k_ratio=" + std::to_string(ratio) + " nu_m=" + std::to_string(nm) +
                              " nu_i=" + std::to_string(ni),
                          a,
                          b);
                }
        const auto& soft = robust.at("soft_inclusion");
        for (double kr : soft.at("k_ratio_ki_over_km").as<std::vector<double>>())
            for (double er : soft.at("E_ratio_Ei_over_Em").as<std::vector<double>>())
                for (double nm : nus)
                    for (double ni : nus) {
                        auto a = m, b = i;
                        a.nu = nm;
                        b.nu = ni;
                        b.k = a.k * kr;
                        b.E = a.E * er;
                        sweep("soft kr=" + std::to_string(kr) + " er=" + std::to_string(er) +
                                  " nu_m=" + std::to_string(nm) + " nu_i=" + std::to_string(ni),
                              a,
                              b);
                    }

        // Independent equal K, G and kappa cases catch reciprocal-contrast divisions by zero.
        for (const std::string field : {"K", "G", "kappa"}) {
            auto a = m, b = i;
            a.nu = 0.25;
            b.nu = 0;
            if (field == "K")
                b.E = a.E * 1.6;
            else if (field == "G")
                b.E = a.E * 0.8;
            else
                b.E = a.E * 2;
            sweep("equal " + field, a, b);
        }

        // The interior of the Levin threshold must return arithmetic CTE even with unequal CTEs.
        // Immediately outside it, equal CTEs avoid amplifying independent inversion roundoff by
        // 1e12. Nondegenerate unequal-CTE Levin is checked by every contrast sweep above.
        for (double offset : {-1.01e-12, -0.99e-12, 0., 0.99e-12, 1.01e-12}) {
            auto b = m;
            b.E = m.E / (1 + offset);
            const double rm = 3 * (1 - 2 * m.nu) / m.E;
            const double ri = 3 * (1 - 2 * b.nu) / b.E;
            const bool arithmetic = std::abs(ri - rm) <= 1e-12 * std::max(ri, rm);
            b.alpha = arithmetic ? i.alpha : m.alpha;
            std::ostringstream label;
            label << "Levin threshold offset=" << std::setprecision(17) << offset;
            for (double f : fractions) {
                cases.run(label.str() + " f=" + std::to_string(f), [&] {
                    const auto actual = chipcae::solve::mixture_vertical(m, b, f);
                    const double expected = f * b.alpha + (1 - f) * m.alpha;
                    for (int d = 0; d < 3; ++d)
                        near(actual.alpha[d], expected, "threshold alpha", tol);
                    compare(m, b, f, tol);
                });
            }
        }

        for (const double er :
             fixture.at("opening_ereg").at("E_i_over_E_m").as<std::vector<double>>()) {
            auto b = i;
            b.E = m.E * er;
            b.nu = fixture.at("opening_ereg").at("nu_i").as<double>();
            b.alpha = fixture.at("opening_ereg").at("alpha_i").as<double>();
            b.k = m.k * soft.at("k_ratio_ki_over_km").at(0).as<double>();
            sweep("opening er=" + std::to_string(er), m, b);
            for (const double f : fractions)
                cases.run("opening SPD er=" + std::to_string(er) + " f=" + std::to_string(f), [&] {
                    const auto actual = chipcae::solve::mixture_vertical(m, b, f);
                    double k[3]{};
                    chipcae::solve::copper_vertical_conductivity(m.k, b.k, f, k);
                    for (int d = 0; d < 3; ++d)
                        require(std::isfinite(k[d]) && std::isfinite(actual.alpha[d]),
                                "finite opening k and alpha");
                    const auto C = stiffness(actual);
                    positive_definite(C, tol);
                    const auto S = inverse(C);
                    std::cout << "opening er=" << er << " f=" << f << " Eperp=" << 1 / S[0][0]
                              << " Ez=" << 1 / S[2][2] << " Gt=" << 1 / S[5][5]
                              << " Gzt=" << 1 / S[3][3] << " nu_t=" << -S[0][1] / S[0][0]
                              << " nu_zt=" << -S[0][2] / S[2][2] << " nu_tz=" << -S[0][2] / S[0][0]
                              << " alpha_perp=" << actual.alpha[0] << " alpha_z=" << actual.alpha[2]
                              << " k_perp=" << k[0] << " k_z=" << k[2] << '\n';
                });
        }

        // M3's 2026-11-23 20:25 supplement supersedes the frozen fixture's 1e-8 gate:
        // finite-stiffness cavity residuals are first order in Ei/Em, bounded by 10*(Ei/Em).
        const auto& cavity = fixture.at("levin_cavity");
        const auto ers = cavity.at("E_i_over_E_m").as<std::vector<double>>();
        for (double er : ers) {
            auto b = i;
            b.E = m.E * er;
            b.alpha = cavity.at("alpha_i").as<double>();
            sweep("cavity er=" + std::to_string(er), m, b);
        }
        for (const double f : fractions) {
            if (f == 1)
                continue; // inclusion-only CTE is covered by byte-exact endpoints above
            cases.run("cavity f=" + std::to_string(f), [&] {
                std::vector<std::array<Real, 3>> residuals;
                for (double er : ers) {
                    const Real cavity_tol = 10 * Real{er};
                    auto b = i;
                    b.E = m.E * er;
                    b.alpha = cavity.at("alpha_i").as<double>();
                    const auto actual = chipcae::solve::mixture_vertical(m, b, f);
                    std::array<Real, 3> residual{};
                    for (int d = 0; d < 3; ++d) {
                        require(std::isfinite(actual.alpha[d]), "finite cavity CTE");
                        residual[d] = std::abs(actual.alpha[d] - m.alpha) /
                                      std::max(std::abs(m.alpha), 1e-30);
                        std::cout << "cavity f=" << f << " er=" << er << " d=" << d
                                  << " residual=" << residual[d] << " limit=" << cavity_tol << '\n';
                        cases.run("cavity gate f=" + std::to_string(f) +
                                      " er=" + std::to_string(er) + " d=" + std::to_string(d),
                                  [&] {
                                      require(residual[d] <= cavity_tol,
                                              "cavity residual exceeds M3 bound 10*(Ei/Em)");
                                  });
                    }
                    residuals.push_back(residual);
                }
                if (f > 0 && ers.size() == 2)
                    for (int d = 0; d < 3; ++d) {
                        require(residuals[0][d] > 0 && residuals[1][d] > 0,
                                "positive regularized cavity residuals for order measurement");
                        const Real order =
                            std::log(residuals[1][d] / residuals[0][d]) / std::log(ers[1] / ers[0]);
                        std::cout << "cavity f=" << f << " d=" << d << " fitted_order=" << order
                                  << '\n';
                        require(std::isfinite(order) && order > 0.9 && order < 1.1,
                                "approximately first-order cavity residual scaling");
                    }
            });
        }
        for (const double f : fractions)
            cases.run("Levin versus Schapery f=" + std::to_string(f), [&] {
                const auto actual = chipcae::solve::mixture_vertical(m, i, f);
                const double schapery =
                    (f * i.E * i.alpha + (1 - f) * m.E * m.alpha) / (f * i.E + (1 - f) * m.E);
                require(std::isfinite(actual.alpha[2]), "finite typical axial CTE");
                std::cout << "typical f=" << f << " Levin_alpha_z=" << actual.alpha[2]
                          << " Schapery_alpha_z=" << schapery
                          << " difference=" << actual.alpha[2] - schapery << '\n';
            });

        const double nan = std::numeric_limits<double>::quiet_NaN();
        const double inf = std::numeric_limits<double>::infinity();
        auto reject_fraction = [&](double f, const std::string& label) {
            cases.run(label + " elastic",
                      [&] { invalid([&] { (void)chipcae::solve::mixture_vertical(m, i, f); }); });
            cases.run(label + " conductivity", [&] {
                double k[3]{};
                invalid([&] { chipcae::solve::copper_vertical_conductivity(m.k, i.k, f, k); });
            });
        };
        for (double f : fixture.at("illegal").at("f").as<std::vector<double>>()) {
            if (f >= 0 && f <= 1) {
                // Frozen illegal.f[1] is 1.0: never reject a valid endpoint or silently replace it.
                cases.run("rounded fixture illegal.f is valid endpoint",
                          [&] { endpoint(m, i, f); });
                std::cout << "fixture notice: illegal.f contains valid endpoint " << f << '\n';
            } else {
                reject_fraction(f, "fixture invalid f=" + std::to_string(f));
            }
        }
        for (double f : {1.001, -1., nan, inf, -inf})
            reject_fraction(f, "additional invalid f=" + std::to_string(f));

        auto reject_elastic = [&](Material bad, const std::string& label) {
            for (double f : fractions) {
                if (f == 0 || f == 1) {
                    // Endpoint delegation must ignore the absent phase, as mixture() does.
                    cases.run(label + " unused phase f=" + std::to_string(f),
                              [&] { endpoint(f == 0 ? m : bad, f == 0 ? bad : i, f); });
                    continue;
                }
                for (bool swap : {false, true})
                    cases.run(label + (swap ? " inclusion" : " matrix") + " f=" + std::to_string(f),
                              [&] {
                                  invalid([&] {
                                      (void)chipcae::solve::mixture_vertical(
                                          swap ? m : bad, swap ? bad : i, f);
                                  });
                              });
            }
        };
        auto reject_k = [&](double bad, const std::string& label) {
            for (double f : fractions)
                for (bool swap : {false, true})
                    cases.run(label + (swap ? " inclusion" : " matrix") + " f=" + std::to_string(f),
                              [&] {
                                  double k[3]{};
                                  invalid([&] {
                                      chipcae::solve::copper_vertical_conductivity(
                                          swap ? m.k : bad, swap ? bad : i.k, f, k);
                                  });
                              });
        };
        for (const auto& j : fixture.at("illegal").at("materials").array_range()) {
            const auto why = j.at("why").as<std::string>();
            auto bad = m;
            if (j.contains("k_m")) {
                reject_k(j.at("k_m").as<double>(), "fixture " + why);
                require(j.at("k_c").as<double>() > 0, "fixture valid inclusion k");
            } else if (j.contains("nu_m")) {
                bad.nu = j.at("nu_m").as<double>();
                require(std::abs(j.at("nu_c").as<double>()) < 0.5, "fixture valid inclusion nu");
                reject_elastic(bad, "fixture " + why);
            } else if (j.contains("E_m")) {
                bad.E = j.at("E_m").as<double>();
                require(j.at("E_c").as<double>() > 0, "fixture valid inclusion E");
                reject_elastic(bad, "fixture " + why);
            } else {
                throw std::runtime_error("unrecognized fixture illegal material");
            }
        }
        for (double value : {0., -1., nan, inf, -inf}) {
            reject_k(value, "invalid k=" + std::to_string(value));
            auto bad = m;
            bad.E = value;
            reject_elastic(bad, "invalid E=" + std::to_string(value));
        }
        for (double value : {-0.5, 0.5, -0.6, 0.6, nan, inf, -inf}) {
            auto bad = m;
            bad.nu = value;
            reject_elastic(bad, "invalid nu=" + std::to_string(value));
        }
        for (double value : {nan, inf, -inf}) {
            auto bad = m;
            bad.alpha = value;
            reject_elastic(bad, "invalid alpha=" + std::to_string(value));
        }
        {
            auto bad = m;
            bad.E = std::numeric_limits<double>::max();
            bad.nu = 0.49; // finite E and nu, but derived plane-strain K overflows double
            reject_elastic(bad, "overflow derived K");
            bad.E = std::numeric_limits<double>::denorm_min();
            bad.nu = 0.3; // derived K and G underflow to zero
            reject_elastic(bad, "underflow derived moduli");
        }
        std::cout << cases.count << " cases, " << cases.failures << " failures\n";
        return cases.failures == 0 ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "test setup error: " << error.what() << '\n';
        return 2;
    }
}
