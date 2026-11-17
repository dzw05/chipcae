#include "chipcae/material/copper_mixture.hpp"
#include "chipcae/solve/mixture.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

int main(int argc, char** argv) {
    Kokkos::ScopeGuard guard(argc, argv);
    using namespace chipcae::material;
    using MS = Kokkos::HostSpace;
    auto model = [](double E, double nu, double alpha) {
        ThermalStrainParams thermal;
        thermal.T_sf = 300;
        return ThermoElastic<MS>{
            ElasticSymmetry::Isotropic,
            TemperatureTable({300, 301}, {{E, E}, {nu, nu}}),
            ThermalStrain<MS>{thermal, TemperatureTable({300, 301}, {{alpha, alpha}})}};
    };
    auto near = [](double a, double b) {
        if (std::abs(a - b) > 1e-12 * std::max({std::abs(a), std::abs(b), 1e-30}))
            throw std::runtime_error("pipeline/material mismatch exceeds 1e-12 relative");
    };
    chipcae::solve::Material a{"matrix", 25e9, .3, 1e-5, 2};
    chipcae::solve::Material b{"copper", 110e9, .34, 17e-6, 400};
    CopperMixture<MS> mixture(model(a.E, a.nu, a.alpha), model(b.E, b.nu, b.alpha));
    for (double f : {0., .01, .3, .75, 1.}) {
        PointInput in;
        in.T_np1 = 301;
        in.point_param = f;
        PointOutput out;
        if (mixture.update({}, in, {}, {}, out) != StatusCode::Ok)
            throw std::runtime_error("material update failed");
        auto pipeline = chipcae::solve::mixture(a, b, f);
        for (int i = 0; i < 6; ++i) {
            if (i < 3)
                near(pipeline.alpha[i], out.thermal_strain[i]);
            for (int j = 0; j < 6; ++j)
                near(pipeline.C(i, j),
                     out.tangent[6 * i + j] /
                         ((i >= 3 ? std::sqrt(2.) : 1.) * (j >= 3 ? std::sqrt(2.) : 1.)));
        }
    }
}
