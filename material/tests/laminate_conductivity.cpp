#include "chipcae/material/copper_laminate.hpp"

#include <Kokkos_Core.hpp>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>

namespace {
int fail(const char* name) {
    std::fprintf(stderr, "FAIL material_laminate_conductivity: %s\n", name);
    return 1;
}
// Accumulate failures so each changed component is observed against the old model.
int parallel_acceptance() {
    int failures = 0;
    auto same_bits = [](double a, double b) {
        return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
    };
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
        expect(same_bits(k[0], k[1]), "conductivity_xy_equal", k[0], k[1]);
        for (int i = 0; i < 2; ++i)
            expect(same_bits(k[i], parallel), "conductivity_xy_unchanged", k[i], parallel);
        for (int i = 0; i < 3; ++i)
            expect(same_bits(k[i], repeated[i]), "conductivity_repeat", repeated[i], k[i]);
    }
    for (double f : {0., .1, .3, .5, .9, 1.}) {
        constexpr double Em = 25e9, Ec = 110e9, nm = .3, nc = .34;
        constexpr double am = 10e-6, ac = 17e-6, km = .3, kc = 390;
        const double m = 1 - f;
        const double E = f * Ec + m * Em, n = f * nc + m * nm;
        const double Gm = Em / (2 * (1 + nm)), Gc = Ec / (2 * (1 + nc));
        const double G = E / (2 * (1 + n));
        // Endpoint expectations use the selected phase; interior formulas stay unchanged.
        const double Gz = f == 0 ? Gm : f == 1 ? Gc : 1 / (f / Gc + m / Gm);
        const double expected[9] = {E, E, E, n, n, n, G, Gz, Gz};
        const double Mm = Em / (1 - nm), Mc = Ec / (1 - nc);
        const double xy = f == 0   ? am
                          : f == 1 ? ac
                                   : (f * Mc * ac + m * Mm * am) / (f * Mc + m * Mm);
        const double z = (f * Ec * ac + m * Em * am) / E;
        double p[9]{}, th[3]{}, k[3]{}, repeated_p[9]{}, repeated_th[3]{}, repeated_k[3]{};
        chipcae::material::copper_laminate(Em, nm, Ec, nc, am, ac, f, p, th);
        chipcae::material::copper_laminate(Em, nm, Ec, nc, am, ac, f, repeated_p, repeated_th);
        chipcae::material::copper_laminate_conductivity(km, kc, f, k);
        chipcae::material::copper_laminate_conductivity(km, kc, f, repeated_k);
        expect(same_bits(p[2], p[0]), "laminate_z_parallel", p[2], p[0]);
        expect(std::abs(th[2] - z) <= 1e-14 * std::abs(z), "laminate_z_force_balance", th[2], z);
        for (int i = 0; i < 9; ++i) {
            if (i != 2)
                expect(same_bits(p[i], expected[i]), "laminate_unchanged", p[i], expected[i]);
            expect(same_bits(p[i], repeated_p[i]), "laminate_repeat", repeated_p[i], p[i]);
        }
        for (int i = 0; i < 3; ++i) {
            if (i < 2) {
                expect(same_bits(th[i], xy), "thermal_xy_unchanged", th[i], xy);
                expect(same_bits(k[i], f * kc + m * km),
                       "conductivity_xy_unchanged",
                       k[i],
                       f * kc + m * km);
            }
            expect(same_bits(th[i], repeated_th[i]), "thermal_repeat", repeated_th[i], th[i]);
            expect(same_bits(k[i], repeated_k[i]), "conductivity_repeat", repeated_k[i], k[i]);
        }
        if (f == 0 || f == 1) {
            expect(same_bits(k[2], f == 0 ? km : kc), "z_endpoints", k[2], f == 0 ? km : kc);
            for (int i = 0; i < 3; ++i)
                expect(
                    same_bits(p[i], f == 0 ? Em : Ec), "modulus_endpoints", p[i], f == 0 ? Em : Ec);
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

    {
        struct Inputs {
            double Em, nm, Ec, nc, am, ac;
        };
        const Inputs fixed[] = {
            {158140741931.79025,
             0.17757178589768871,
             84315037342.432434,
             0.18742227319584831,
             9.4640751552894535e-06,
             2.4293853342851421e-05},
            {177795642178.68613,
             0.22971190759452192,
             8649191984.9544163,
             0.19914270823773,
             2.6673832145764693e-05,
             8.5914695768405087e-06},
            {97530823268.158463,
             0.41780894308685013,
             128081563629.78484,
             0.30323890330362008,
             1.5210546026799818e-05,
             7.0621854608797717e-06},
            {174592178418.05817,
             0.15672390404360254,
             159345908120.14728,
             0.27300894871593839,
             1.6972811730378521e-05,
             2.2136312797454289e-05},
            {27641452158.494015,
             0.14774246272816904,
             90791765865.063095,
             0.10735847994585447,
             1.1176045299704669e-05,
             2.7429383389424128e-05},
            {25e9, 0.3, 110e9, 0.34, 1e-5, 17e-6},
        };
        auto endpoint = [](const Inputs& input, double f, const char* context) {
            const auto [Em, nm, Ec, nc, am, ac] = input;
            const double Eb = f == 0 ? Em : Ec;
            const double nb = f == 0 ? nm : nc;
            const double ab = f == 0 ? am : ac;
            // Pure-phase shear modulus definition, independent of the laminate helper.
            const double Gb = Eb / (2 * (1 + nb));
            const double expected[9] = {Eb, Eb, Eb, nb, nb, nb, Gb, Gb, Gb};
            double p[9]{}, th[3]{};
            chipcae::material::copper_laminate(Em, nm, Ec, nc, am, ac, f, p, th);
            for (int i = 0; i < 9; ++i) {
                if (!(p[i] == expected[i])) {
                    char name[128];
                    std::snprintf(name, sizeof(name), "%s_f%.0f_p%d_exact", context, f, i);
                    std::fprintf(stderr, "actual=%.17g expected=%.17g\n", p[i], expected[i]);
                    return fail(name);
                }
            }
            for (int i = 0; i < 3; ++i) {
                if (!(th[i] == ab)) {
                    char name[128];
                    std::snprintf(name, sizeof(name), "%s_f%.0f_th%d_exact", context, f, i);
                    std::fprintf(stderr, "actual=%.17g expected=%.17g\n", th[i], ab);
                    return fail(name);
                }
            }
            return 0;
        };

        // Near endpoints must retain the interior formulas, without snapping to a phase.
        for (const auto& [Em, nm, Ec, nc, am, ac] : fixed) {
            for (double f : {0.3, 1e-300, 1 - 1e-16}) {
                const double m = 1 - f;
                const double Gm = Em / (2 * (1 + nm)), Gc = Ec / (2 * (1 + nc));
                const double E = f * Ec + m * Em, n = f * nc + m * nm;
                const double G = E / (2 * (1 + n)), Gz = 1 / (f / Gc + m / Gm);
                const double expected[9] = {E, E, E, n, n, n, G, Gz, Gz};
                const double Mm = Em / (1 - nm), Mc = Ec / (1 - nc);
                const double xy = (f * Mc * ac + m * Mm * am) / (f * Mc + m * Mm);
                const double z = (f * Ec * ac + m * Em * am) / E;
                double p[9]{}, th[3]{};
                chipcae::material::copper_laminate(Em, nm, Ec, nc, am, ac, f, p, th);
                for (int i = 0; i < 9; ++i)
                    if (!(p[i] == expected[i]))
                        return fail("interior_or_near_endpoint_mechanical_exact");
                if (!(th[0] == xy && th[1] == xy && th[2] == z))
                    return fail("interior_or_near_endpoint_thermal_exact");
            }
        }
        for (const auto& input : fixed)
            for (double f : {0., 1.})
                if (endpoint(input, f, "fixed_endpoint") != 0)
                    return 1;

        const Inputs homogeneous{158140741931.79025,
                                 0.17757178589768871,
                                 158140741931.79025,
                                 0.17757178589768871,
                                 9.4640751552894535e-06,
                                 9.4640751552894535e-06};
        for (double f : {0., 1.})
            if (endpoint(homogeneous, f, "homogeneous_endpoint") != 0)
                return 1;

        std::mt19937_64 g(1);
        std::uniform_real_distribution<double> modulus(1e9, 2e11);
        std::uniform_real_distribution<double> poisson(0.1, 0.45);
        std::uniform_real_distribution<double> expansion(1e-6, 3e-5);
        for (int sample = 0; sample < 10000; ++sample) {
            const double Em = modulus(g);
            const double nm = poisson(g);
            const double Ec = modulus(g);
            const double nc = poisson(g);
            const double am = expansion(g);
            const double ac = expansion(g);
            const Inputs input{Em, nm, Ec, nc, am, ac};
            for (double f : {0., 1.})
                if (endpoint(input, f, "random_endpoint") != 0)
                    return 1;
        }
    }

    return failures == 0 ? 0 : 1;
}
