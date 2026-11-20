#pragma once
#include "mesh.hpp"
namespace chipcae::proto_cutfem {
struct MaterialParamsSoA {
    std::vector<double> E, nu, alpha, k;
    double T_stress_free = 0;
};
template <class Params>
CHIPCAE_INLINE_FUNCTION int linear_thermoelastic_plane_strain(
    const Params& p, int mat, const double eps[3], double dT, double sig[3], double D[3][3]) {
    if (mat < 0 || mat >= int(p.E.size()) || p.E[mat] <= 0 || p.nu[mat] <= -1 || p.nu[mat] >= 0.5)
        return 1;
    double E = p.E[mat], nu = p.nu[mat], mu = E / (2 * (1 + nu)),
           lam = E * nu / ((1 + nu) * (1 - 2 * nu));
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            D[i][j] = 0;
    D[0][0] = D[1][1] = lam + 2 * mu;
    D[0][1] = D[1][0] = lam;
    D[2][2] = mu;
    double beta = (3 * lam + 2 * mu) * p.alpha[mat];
    sig[0] = D[0][0] * eps[0] + lam * eps[1] - beta * dT;
    sig[1] = lam * eps[0] + D[1][1] * eps[1] - beta * dT;
    sig[2] = mu * eps[2];
    return 0;
}
template <class Params>
CHIPCAE_INLINE_FUNCTION double thermal_conductivity(const Params& p, int mat) {
    return p.k[mat];
}
template <class Params>
CHIPCAE_INLINE_FUNCTION double thermal_beta(const Params& p, int m) {
    return p.E[m] / (1 - 2 * p.nu[m]) * p.alpha[m];
}
} // namespace chipcae::proto_cutfem
