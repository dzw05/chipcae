#include "constitutive.hpp"

#include <iostream>
using namespace chipcae;
int main() {
    Kokkos::ScopeGuard guard;
    using namespace material;
    solve::Material matrix{"matrix", 25e9, .3, 1e-5, 2}, copper{"copper", 110e9, .34, 17e-6, 400};
    TemperatureTable em(
        {300}, {{matrix.E}, {matrix.nu}}, Interpolation::Linear, Extrapolation::Clamp);
    TemperatureTable ec(
        {300}, {{copper.E}, {copper.nu}}, Interpolation::Linear, Extrapolation::Clamp);
    TemperatureTable am({300}, {{matrix.alpha}}, Interpolation::Linear, Extrapolation::Clamp);
    TemperatureTable ac({300}, {{copper.alpha}}, Interpolation::Linear, Extrapolation::Clamp);
    CopperMixture<Kokkos::HostSpace> model(
        ThermoElastic<Kokkos::HostSpace>(
            ElasticSymmetry::Isotropic, em, ThermalStrain<Kokkos::HostSpace>({}, am)),
        ThermoElastic<Kokkos::HostSpace>(
            ElasticSymmetry::Isotropic, ec, ThermalStrain<Kokkos::HostSpace>({}, ac)));
    for (double f : {0., .3, 1.}) {
        auto response = solve::mixture(matrix, copper, f);
        PointInput input;
        input.point_param = f;
        input.T_np1 = 1;
        PointOutput output;
        if (model.update({}, input, {}, {}, output) != StatusCode::Ok)
            return 1;
        for (int i = 0; i < 6; ++i)
            for (int j = 0; j < 6; ++j) {
                double expected = output.tangent[6 * i + j] /
                                  ((i < 3 ? 1 : std::sqrt(2.)) * (j < 3 ? 1 : std::sqrt(2.)));
                if (std::abs(response.C(i, j) - expected) >
                    1e-12 * std::max(1., std::abs(expected)))
                    return 2;
            }
        for (int i = 0; i < 3; ++i)
            if (std::abs(response.alpha[i] - output.thermal_strain[i]) >
                1e-12 * std::abs(output.thermal_strain[i]))
                return 3;
    }
    std::cout << "pipeline C/alpha agree with CopperMixture within 1e-12\n";
}
