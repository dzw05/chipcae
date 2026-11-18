#include "chipcae/material/copper_laminate.hpp"

#include <Kokkos_Core.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {
int fail(const char* name) {
    std::fprintf(stderr, "FAIL material_laminate_conductivity: %s\n", name);
    return 1;
}
} // namespace

int main(int argc, char** argv) {
    Kokkos::ScopeGuard guard(argc, argv);

    {
        double k[3] = {};
        chipcae::material::copper_laminate_conductivity(0.3, 390.0, 0.0, k);
        if (!(k[0] == 0.3 && k[1] == 0.3 && k[2] == 0.3))
            return fail("matrix_limit");
    }

    {
        double k[3] = {};
        chipcae::material::copper_laminate_conductivity(0.3, 390.0, 1.0, k);
        if (!(k[0] == 390.0 && k[1] == 390.0 && k[2] == 390.0))
            return fail("copper_limit");
    }

    {
        double k[3] = {};
        chipcae::material::copper_laminate_conductivity(1.0, 3.0, 0.5, k);
        if (!(k[0] == 2.0 && k[1] == 2.0 && k[2] == 1.5))
            return fail("half_exact");
    }

    {
        constexpr double km = 0.3;
        constexpr double kc = 390.0;
        constexpr double f = 0.12;
        const double exy = f * kc + (1 - f) * km;
        const double ez = km * kc / (f * km + (1 - f) * kc);
        double k[3] = {};
        chipcae::material::copper_laminate_conductivity(km, kc, f, k);
        if (!(k[0] == k[1] && std::abs(k[0] - exy) <= 1e-15 * std::max(1.0, std::abs(exy)) &&
              std::abs(k[2] - ez) <= 1e-15 * std::max(1.0, std::abs(ez))))
            return fail("uncut_chain_fraction");
    }

    {
        double k[3] = {};
        chipcae::material::copper_laminate_conductivity(0.3, 390.0, 0.0, k);
        chipcae::material::copper_laminate_conductivity(0.3, 390.0, 1.0, k);
        if (!(k[0] == 390.0 && k[1] == 390.0 && k[2] == 390.0))
            return fail("overwrite");
    }

    return 0;
}
