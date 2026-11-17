#pragma once

// Shared M7 laminate rules. Thermal inputs may be CTEs or integrated strains.
#if defined(__CUDACC__) || defined(__HIPCC__)
#define CHIPCAE_LAMINATE_INLINE __host__ __device__ inline
#else
#define CHIPCAE_LAMINATE_INLINE inline
#endif
namespace chipcae::material {
CHIPCAE_LAMINATE_INLINE void copper_laminate(double Em,
                                             double nm,
                                             double Ec,
                                             double nc,
                                             double am,
                                             double ac,
                                             double f,
                                             double* p,
                                             double* th) {
    const double m = 1 - f;
    const double Gm = Em / (2 * (1 + nm)), Gc = Ec / (2 * (1 + nc));
    const double E = f * Ec + m * Em, n = f * nc + m * nm;
    const double G = E / (2 * (1 + n)), Gz = 1 / (f / Gc + m / Gm);
    const double values[9] = {E, E, 1 / (f / Ec + m / Em), n, n, n, G, Gz, Gz};
    for (int i = 0; i < 9; ++i)
        p[i] = values[i];
    const double Mm = Em / (1 - nm), Mc = Ec / (1 - nc);
    th[0] = th[1] = (f * Mc * ac + m * Mm * am) / (f * Mc + m * Mm);
    th[2] =
        f * (ac - 2 * nc / (1 - nc) * (th[0] - ac)) + m * (am - 2 * nm / (1 - nm) * (th[0] - am));
}
} // namespace chipcae::material
#undef CHIPCAE_LAMINATE_INLINE
