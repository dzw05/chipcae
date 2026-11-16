// Confirms one CPU build exercises both Kokkos::Serial and Kokkos::OpenMP.
#include <Kokkos_Core.hpp>
#include <cstdint>
#include <iostream>

int main(int argc, char** argv) {
#if !defined(KOKKOS_ENABLE_SERIAL) || !defined(KOKKOS_ENABLE_OPENMP)
    std::cerr << "exec_spaces: this build must enable Kokkos Serial and OpenMP\n";
    return 1;
#endif
    Kokkos::ScopeGuard guard(argc, argv);

    const int n = 64;
    Kokkos::View<int*, Kokkos::HostSpace> serial_out("serial_out", static_cast<std::size_t>(n));
    Kokkos::View<int*, Kokkos::HostSpace> openmp_out("openmp_out", static_cast<std::size_t>(n));
    Kokkos::deep_copy(serial_out, 0);
    Kokkos::deep_copy(openmp_out, 0);

    Kokkos::parallel_for(
        "chipcae::material::exec_spaces_serial",
        Kokkos::RangePolicy<Kokkos::Serial>(0, n),
        KOKKOS_LAMBDA(const int i) { serial_out(i) = i + 1; });
    Kokkos::fence();

    Kokkos::parallel_for(
        "chipcae::material::exec_spaces_openmp",
        Kokkos::RangePolicy<Kokkos::OpenMP>(0, n),
        KOKKOS_LAMBDA(const int i) { openmp_out(i) = 2 * (i + 1); });
    Kokkos::fence();

    for (int i = 0; i < n; ++i) {
        if (serial_out(i) != i + 1 || openmp_out(i) != 2 * (i + 1)) {
            std::cerr << "exec_spaces: mismatch at " << i << "\n";
            return 1;
        }
    }
    std::cout << "exec_spaces: Kokkos::Serial and Kokkos::OpenMP both ran\n";
    return 0;
}
