// Exercise the real ThermoElastic point update on one requested Kokkos backend.
#include "chipcae/material/thermo_elastic.hpp"

#include <Kokkos_Core.hpp>
#include <iostream>
#include <string_view>

using namespace chipcae::material;

template <class ExecutionSpace>
bool run_update() {
    TemperatureTable table({0.0}, {{1.0e9}, {0.3}}, Interpolation::Linear, Extrapolation::Clamp);
    ThermoElastic<Kokkos::HostSpace> model(ElasticSymmetry::Isotropic, table);
    Kokkos::View<Real* [6], Kokkos::HostSpace> stresses("stresses", 16);
    Kokkos::View<StatusCode*, Kokkos::HostSpace> statuses("statuses", 16);
    StepContext ctx{};
    ctx.dt = 1.0;
    Kokkos::parallel_for(
        "thermoelastic_update", Kokkos::RangePolicy<ExecutionSpace>(0, 16), KOKKOS_LAMBDA(int i) {
            PointInput in{};
            in.strain_np1[0] = 1e-3;
            in.T_np1 = 0;
            PointOutput out{};
            model.update(ctx, in, ConstStateRef{}, StateRef{}, out);
            statuses(i) = out.status.code;
            stresses(i, 0) = out.stress[0];
        });
    Kokkos::fence();
    for (int i = 0; i < 16; ++i)
        if (statuses(i) != StatusCode::Ok || !(stresses(i, 0) > 0))
            return false;
    return true;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "exec_spaces: expected Serial or OpenMP\n";
        return 2;
    }
    const std::string_view which = argv[1];
    Kokkos::ScopeGuard guard(argc, argv);
    bool ok = false;
    if (which == "Serial") {
#if defined(KOKKOS_ENABLE_SERIAL)
        ok = run_update<Kokkos::Serial>();
#else
        std::cerr << "SKIP: Kokkos::Serial is not enabled in this build\n";
        return 0;
#endif
    } else if (which == "OpenMP") {
#if defined(KOKKOS_ENABLE_OPENMP)
        ok = run_update<Kokkos::OpenMP>();
#else
        std::cerr << "SKIP: Kokkos::OpenMP is not enabled in this build\n";
        return 0;
#endif
    } else {
        std::cerr << "exec_spaces: expected Serial or OpenMP\n";
        return 2;
    }
    if (!ok) {
        std::cerr << "exec_spaces: ThermoElastic update failed\n";
        return 1;
    }
    return 0;
}
