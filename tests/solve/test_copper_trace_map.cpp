#include "chipcae/material/copper_laminate.hpp"
#include "chipcae/solve/mixture.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {
void near(double actual, double expected, double tolerance, const std::string& context) {
    const double denominator = std::max({std::abs(actual), std::abs(expected), 1e-30});
    if (!std::isfinite(actual) || std::abs(actual - expected) / denominator > tolerance)
        throw std::runtime_error(context + ": actual=" + std::to_string(actual) +
                                 " expected=" + std::to_string(expected));
}
using Matrix = std::array<std::array<double, 6>, 6>;

// Independent, pivoted Gauss-Jordan inverse; do not use the adapter's MFEM inverse.
Matrix inverse(Matrix a) {
    Matrix result{};
    for (std::size_t i = 0; i < 6; ++i)
        result[i][i] = 1;
    for (std::size_t k = 0; k < 6; ++k) {
        std::size_t pivot = k;
        for (std::size_t i = k + 1; i < 6; ++i)
            if (std::abs(a[i][k]) > std::abs(a[pivot][k]))
                pivot = i;
        if (!(std::abs(a[pivot][k]) > 0))
            throw std::runtime_error("singular laminate compliance");
        std::swap(a[k], a[pivot]);
        std::swap(result[k], result[pivot]);
        const double diagonal = a[k][k];
        for (std::size_t j = 0; j < 6; ++j) {
            a[k][j] /= diagonal;
            result[k][j] /= diagonal;
        }
        for (std::size_t i = 0; i < 6; ++i) {
            if (i == k)
                continue;
            const double factor = a[i][k];
            for (std::size_t j = 0; j < 6; ++j) {
                a[i][j] -= factor * a[k][j];
                result[i][j] -= factor * result[k][j];
            }
        }
    }
    return result;
}
} // namespace

int main() {
    chipcae::solve::Material base, metal;
    base.id = "fr4_eq";
    base.E = 25e9;
    base.nu = .3;
    base.alpha = 1e-5;
    base.k = .3;
    metal.id = "cu";
    metal.E = 110e9;
    metal.nu = .34;
    metal.alpha = 17e-6;
    metal.k = 390;
    // Lock the shared helper to the parallel conductivity expressions in all three directions.
    for (double f : {0., .12, .3, .5, 1.}) {
        const double kxy = f * metal.k + (1 - f) * base.k;
        const double kz = f * metal.k + (1 - f) * base.k;
        std::array<double, 3> actual{};
        chipcae::material::copper_laminate_conductivity(base.k, metal.k, f, actual.data());
        const std::array<double, 3> expected{kxy, kxy, kz};
        for (std::size_t d = 0; d < actual.size(); ++d)
            if (!(actual[d] == expected[d])) {
                std::ostringstream message;
                message << "exact parallel conductivity: f=" << f << " component=" << d
                        << std::hexfloat << " actual=" << actual[d] << " expected=" << expected[d];
                throw std::runtime_error(message.str());
            }
    }
    for (double f : {0., .3, 1.}) {
        double conductivity[3]{};
        chipcae::material::copper_laminate_conductivity(base.k, metal.k, f, conductivity);
        const double kxy = f * metal.k + (1 - f) * base.k;
        const double kz = f * metal.k + (1 - f) * base.k;
        for (int d = 0; d < 3; ++d)
            near(conductivity[d], d == 2 ? kz : kxy, 1e-14, "conductivity formula");

        double p[9]{}, thermal[3]{};
        chipcae::material::copper_laminate(
            base.E, base.nu, metal.E, metal.nu, base.alpha, metal.alpha, f, p, thermal);
        Matrix compliance{};
        for (std::size_t i = 0; i < 3; ++i)
            compliance[i][i] = 1 / p[i];
        compliance[0][1] = compliance[1][0] = -p[3] / p[0];
        compliance[0][2] = compliance[2][0] = -p[4] / p[0];
        compliance[1][2] = compliance[2][1] = -p[5] / p[1];
        // Engineering shear order: yz, xz, xy.
        compliance[3][3] = 1 / p[8];
        compliance[4][4] = 1 / p[7];
        compliance[5][5] = 1 / p[6];
        const auto stiffness = inverse(compliance);
        const auto actual = chipcae::solve::mixture(base, metal, f);
        for (int i = 0; i < 6; ++i) {
            for (int j = 0; j < 6; ++j)
                near(actual.C(i, j), stiffness[i][j], 1e-12, "all 36 stiffness entries");
            if (i < 3)
                near(actual.alpha[i], thermal[i], 1e-12, "all three thermal coefficients");
        }
        if (f < .01 || f > .99) {
            const auto& isotropic = f < .01 ? base : metal;
            const double mu = isotropic.E / (2 * (1 + isotropic.nu));
            const double lambda =
                isotropic.E * isotropic.nu / ((1 + isotropic.nu) * (1 - 2 * isotropic.nu));
            for (int i = 0; i < 6; ++i)
                for (int j = 0; j < 6; ++j) {
                    const double expected = i < 3 && j < 3 ? lambda + (i == j ? 2 * mu : 0)
                                            : i == j       ? mu
                                                           : 0;
                    near(actual.C(i, j), expected, 1e-12, "isotropic endpoint stiffness");
                }
            for (int d = 0; d < 3; ++d) {
                near(actual.alpha[d], isotropic.alpha, 1e-12, "isotropic endpoint expansion");
                near(conductivity[d], isotropic.k, 1e-14, "isotropic endpoint conductivity");
            }
        }
    }
}
