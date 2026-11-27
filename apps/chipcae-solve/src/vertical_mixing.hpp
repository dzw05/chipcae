#pragma once

#include "chipcae/solve/mixture.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace chipcae::solve {
namespace vertical_detail {
inline constexpr double levin_equal_bulk_rel_tol = 1e-12;

inline void validate_fraction(double f) {
    if (!std::isfinite(f) || f < 0 || f > 1)
        throw InputError(ErrorCode::INPUT_INVALID, "invalid copper fraction");
}

struct Moduli {
    double K, G;
};

inline Moduli validate_phase(const Material& phase, const char* name) {
    if (!std::isfinite(phase.E) || phase.E <= 0 || !std::isfinite(phase.nu) ||
        std::abs(phase.nu) >= 0.5 || !std::isfinite(phase.alpha))
        throw InputError(ErrorCode::INPUT_INVALID,
                         std::string("invalid vertical elastic phase: ") + name);
    const double G = phase.E / (2 * (1 + phase.nu));
    const double K = phase.E / (2 * (1 + phase.nu) * (1 - 2 * phase.nu));
    if (!std::isfinite(K) || K <= 0 || !std::isfinite(G) || G <= 0)
        throw InputError(ErrorCode::INPUT_INVALID,
                         std::string("invalid vertical derived K/G: ") + name);
    return {K, G};
}
} // namespace vertical_detail

// Column axis is z. K denotes plane-strain bulk, never the 3D bulk used by Levin.
inline Constitutive mixture_vertical(const Material& matrix, const Material& inclusion, double f) {
    // Preserve legacy endpoint behavior, including ignoring the absent phase.
    if (f == 0 || f == 1)
        return mixture(matrix, inclusion, f);
    vertical_detail::validate_fraction(f);
    const auto m = vertical_detail::validate_phase(matrix, "matrix");
    const auto i = vertical_detail::validate_phase(inclusion, "inclusion");

    const double v = 1 - f, dn = inclusion.nu - matrix.nu;
    const double den = f / m.K + v / i.K + 1 / m.G;
    // Hashin--Rosen (1964), with the 1965 erratum; Hill (1964).
    const double Ez = f * inclusion.E + v * matrix.E + 4 * f * v * dn * dn / den;
    const double nuzt = f * inclusion.nu + v * matrix.nu + f * v * dn * (1 / m.K - 1 / i.K) / den;
    const double Gzt = m.G * (v * m.G + (1 + f) * i.G) / ((1 + f) * m.G + v * i.G);
    // Matrix-host HS bounds: lower for hard inclusions, upper for soft inclusions.
    // Contrast-multiplied forms also cover equal moduli without reciprocal singularities.
    const double dK = i.K - m.K, dG = i.G - m.G;
    const double Kt = m.K + f * dK * (m.K + m.G) / (m.K + m.G + v * dK);
    const double Gt = m.G + f * dG / (1 + v * dG * (m.K + 2 * m.G) / (2 * m.G * (m.K + m.G)));
    // Transverse isotropy identities (Hashin 1983); reciprocal transverse/axial nu.
    const double factor = 1 + 4 * Kt * nuzt * nuzt / Ez;
    const double Et = 4 * Kt * Gt / (Kt + factor * Gt);
    const double nut = (Kt - factor * Gt) / (Kt + factor * Gt);
    const double nutz = nuzt * Et / Ez;
    const double p[9] = {Et, Et, Ez, nut, nutz, nutz, Gt, Gzt, Gzt};
    // Match mixture.hpp's engineering shear ordering (xx, yy, zz, yz, xz, xy).
    mfem::DenseMatrix compliance(6);
    compliance = 0.0;
    for (int d = 0; d < 3; ++d)
        compliance(d, d) = 1 / p[d];
    compliance(0, 1) = compliance(1, 0) = -p[3] / p[0];
    compliance(0, 2) = compliance(2, 0) = -p[4] / p[0];
    compliance(1, 2) = compliance(2, 1) = -p[5] / p[1];
    compliance(3, 3) = 1 / p[8];
    compliance(4, 4) = 1 / p[7];
    compliance(5, 5) = 1 / p[6];
    Constitutive result;
    mfem::DenseMatrixInverse inverse(compliance);
    inverse.GetInverseMatrix(result.C);

    // Levin (1967), anisotropic form of Rosen--Hashin (1970). Contract the inverse
    // of the actual stiffness so thermal expansion uses the same constitutive law.
    mfem::DenseMatrixInverse stiffness_inverse(result.C);
    stiffness_inverse.GetInverseMatrix(compliance);
    const double rm = 3 * (1 - 2 * matrix.nu) / matrix.E;
    const double ri = 3 * (1 - 2 * inclusion.nu) / inclusion.E;
    const double amean = f * inclusion.alpha + v * matrix.alpha;
    const double diff = ri - rm;
    const double meanS = (f * ri + v * rm) / 3;
    for (int d = 0; d < 3; ++d) {
        const double row = compliance(d, 0) + compliance(d, 1) + compliance(d, 2);
        result.alpha[d] =
            std::abs(diff) <= vertical_detail::levin_equal_bulk_rel_tol * std::max(ri, rm)
                ? amean
                : amean + 3 * (inclusion.alpha - matrix.alpha) / diff * (row - meanS);
    }
    return result;
}

inline void copper_vertical_conductivity(double km, double ki, double f, double* k) {
    vertical_detail::validate_fraction(f);
    // The legacy conductivity law requires both phases even at endpoints (unlike mixture()).
    if (!std::isfinite(km) || km <= 0 || !std::isfinite(ki) || ki <= 0)
        throw InputError(ErrorCode::INPUT_INVALID, "invalid vertical conductivity km/ki");
    if (f == 0 || f == 1) {
        material::copper_laminate_conductivity(km, ki, f, k);
        return;
    }
    // Rayleigh / Maxwell Garnett cylinder law; scale to avoid overflowing ki+km.
    const double scale = std::max(km, ki), m = km / scale, i = ki / scale;
    k[0] = k[1] = km * (((1 + f) * i + (1 - f) * m) / ((1 - f) * i + (1 + f) * m));
    k[2] = f * ki + (1 - f) * km;
}
} // namespace chipcae::solve
