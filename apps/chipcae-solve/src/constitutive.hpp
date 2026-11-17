#pragma once
#include "chipcae/solve/case_loader.hpp"

#include <cmath>
#include <mfem.hpp>
#ifdef CHIPCAE_SOLVE_MATERIAL
#include "chipcae/material/copper_mixture.hpp"
#endif
namespace chipcae::solve {
using mfem::DenseMatrix;
using mfem::Vector;
// Material-library laminate engineering response, with z normal to the stack. C uses engineering
// shear strain (xx, yy, zz, yz, xz, xy), unlike the material-point tensor Voigt API.
struct Constitutive {
    DenseMatrix C;
    Vector alpha;
    Constitutive() : C(6), alpha(3) {}
};
inline Constitutive mixture(const Material& matrix, const Material& metal, double f) {
#ifdef CHIPCAE_SOLVE_MATERIAL
    using namespace chipcae::material;
    auto component = [](const Material& m) {
        TemperatureTable elastic(
            {300}, {{m.E}, {m.nu}}, Interpolation::Linear, Extrapolation::Clamp);
        TemperatureTable cte({300}, {{m.alpha}}, Interpolation::Linear, Extrapolation::Clamp);
        ThermalStrainParams params;
        return ThermoElastic<Kokkos::HostSpace>(
            ElasticSymmetry::Isotropic, elastic, ThermalStrain<Kokkos::HostSpace>(params, cte));
    };
    CopperMixture<Kokkos::HostSpace> model(component(matrix), component(metal));
    PointInput in;
    in.T_np1 = 1;
    in.point_param = f;
    PointOutput out;
    if (model.update({}, in, {}, {}, out) != StatusCode::Ok)
        throw std::runtime_error("CopperMixture failed");
    Constitutive result;
    // Material API uses Mandel (xx yy zz yz xz xy); assembly uses engineering
    // (xx yy zz yz xz xy). Convert both the order and shear normalization.
    const int order[6] = {0, 1, 2, 3, 4, 5};
    for (int i = 0; i < 6; ++i)
        for (int j = 0; j < 6; ++j)
            result.C(i, j) = out.tangent[6 * order[i] + order[j]] /
                             ((i < 3 ? 1 : std::sqrt(2.)) * (j < 3 ? 1 : std::sqrt(2.)));
    for (int i = 0; i < 3; ++i)
        result.alpha[i] = out.thermal_strain[i];
#else
    if (f != 0)
        throw InputError(ErrorCode::NOT_IMPLEMENTED,
                         "copper mixtures require the material library (Kokkos)");
    (void)metal;
    Constitutive result;
    result.C = 0.0;
    double mu = matrix.E / (2 * (1 + matrix.nu)),
           lambda = matrix.E * matrix.nu / ((1 + matrix.nu) * (1 - 2 * matrix.nu));
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            result.C(i, j) = lambda + (i == j ? 2 * mu : 0);
    for (int i = 3; i < 6; ++i)
        result.C(i, i) = mu;
    result.alpha = matrix.alpha;
#endif
    return result;
}
} // namespace chipcae::solve
