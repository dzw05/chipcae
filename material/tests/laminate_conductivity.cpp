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
// Accumulate failures so each changed component is observed against the old model.
int parallel_acceptance() {
    int failures = 0;
    auto expect = [&](bool ok, const char* name, double actual, double expected) {
        if (!ok) {
            std::fprintf(stderr, "FAIL %s: actual=%.17g expected=%.17g\n", name, actual, expected);
            ++failures;
        }
    };
    for (bool public_example : {false, true}) {
        const double km = public_example ? .294 : .3;
        const double kc = public_example ? 400 : 390;
        const double f = public_example ? .51 : .12;
        const double expected = public_example ? 204.14406 : 47.064;
        const double parallel = f * kc + (1 - f) * km;
        double k[3]{}, repeated[3]{};
        chipcae::material::copper_laminate_conductivity(km, kc, f, k);
        chipcae::material::copper_laminate_conductivity(km, kc, f, repeated);
        expect(std::abs(k[2] - expected) <= 1e-14 * expected,
               public_example ? "z_parallel_public_example" : "z_parallel_f012",
               k[2],
               expected);
        expect(k[0] == k[1], "conductivity_xy_equal", k[0], k[1]);
        for (int i = 0; i < 2; ++i)
            expect(k[i] == parallel, "conductivity_xy_unchanged", k[i], parallel);
        for (int i = 0; i < 3; ++i)
            expect(k[i] == repeated[i], "conductivity_repeat", repeated[i], k[i]);
    }
    for (double f : {0., .1, .3, .5, .9, 1.}) {
        constexpr double Em = 25e9, Ec = 110e9, nm = .3, nc = .34;
        constexpr double am = 10e-6, ac = 17e-6, km = .3, kc = 390;
        const double m = 1 - f;
        const double E = f * Ec + m * Em, n = f * nc + m * nm;
        const double Gm = Em / (2 * (1 + nm)), Gc = Ec / (2 * (1 + nc));
        const double G = E / (2 * (1 + n)), Gz = 1 / (f / Gc + m / Gm);
        const double expected[9] = {E, E, E, n, n, n, G, Gz, Gz};
        const double Mm = Em / (1 - nm), Mc = Ec / (1 - nc);
        const double xy = (f * Mc * ac + m * Mm * am) / (f * Mc + m * Mm);
        const double z = (f * Ec * ac + m * Em * am) / E;
        double p[9]{}, th[3]{}, k[3]{}, repeated_p[9]{}, repeated_th[3]{}, repeated_k[3]{};
        chipcae::material::copper_laminate(Em, nm, Ec, nc, am, ac, f, p, th);
        chipcae::material::copper_laminate(Em, nm, Ec, nc, am, ac, f, repeated_p, repeated_th);
        chipcae::material::copper_laminate_conductivity(km, kc, f, k);
        chipcae::material::copper_laminate_conductivity(km, kc, f, repeated_k);
        expect(p[2] == p[0], "laminate_z_parallel", p[2], p[0]);
        expect(std::abs(th[2] - z) <= 1e-14 * std::abs(z), "laminate_z_force_balance", th[2], z);
        for (int i = 0; i < 9; ++i) {
            if (i != 2)
                expect(p[i] == expected[i], "laminate_unchanged", p[i], expected[i]);
            expect(p[i] == repeated_p[i], "laminate_repeat", repeated_p[i], p[i]);
        }
        for (int i = 0; i < 3; ++i) {
            if (i < 2) {
                expect(th[i] == xy, "thermal_xy_unchanged", th[i], xy);
                expect(k[i] == f * kc + m * km, "conductivity_xy_unchanged", k[i], f * kc + m * km);
            }
            expect(th[i] == repeated_th[i], "thermal_repeat", repeated_th[i], th[i]);
            expect(k[i] == repeated_k[i], "conductivity_repeat", repeated_k[i], k[i]);
        }
        if (f == 0 || f == 1) {
            expect(k[2] == (f == 0 ? km : kc), "z_endpoints", k[2], f == 0 ? km : kc);
            for (int i = 0; i < 3; ++i)
                expect(p[i] == (f == 0 ? Em : Ec), "modulus_endpoints", p[i], f == 0 ? Em : Ec);
            const double phase = f == 0 ? am : ac;
            expect(std::abs(th[2] - phase) <= 1e-15 * std::abs(phase),
                   "thermal_z_endpoints",
                   th[2],
                   phase);
        }
        if (f == .3)
            std::printf(
                "f=0.3 compiled helper: kz=%.17g Ez=%.17g alpha_z=%.17g\n", k[2], p[2], th[2]);
    }
    return failures;
}
} // namespace

int main(int argc, char** argv) {
    Kokkos::ScopeGuard guard(argc, argv);
    const int failures = parallel_acceptance();

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
        if (!(k[0] == 2.0 && k[1] == 2.0 && k[2] == 2.0))
            return fail("half_exact");
    }

    {
        constexpr double km = 0.3;
        constexpr double kc = 390.0;
        constexpr double f = 0.12;
        const double exy = f * kc + (1 - f) * km;
        const double ez = f * kc + (1 - f) * km;
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

    return failures == 0 ? 0 : 1;
}
