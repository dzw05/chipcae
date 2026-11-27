// SPDX-License-Identifier: Apache-2.0
#if defined(CHIPCAE_PROBE_POSIX_CLOCK)
#define _POSIX_C_SOURCE 200809L
#endif

#include <mpi.h>
#include <stdio.h>
#include <time.h>

static int timestamp(struct timespec* value) {
#if defined(CHIPCAE_PROBE_TIMESPEC_GET)
    return timespec_get(value, TIME_UTC) == TIME_UTC;
#elif defined(CHIPCAE_PROBE_POSIX_CLOCK)
    return clock_gettime(CLOCK_MONOTONIC, value) == 0;
#else
#error No supported probe clock
#endif
}

int main(int argc, char** argv) {
    struct timespec begin;
    struct timespec end;
    if (!timestamp(&begin)) {
        fputs("probe clock failed before MPI_Init\n", stderr);
        return 1;
    }
    if (MPI_Init(&argc, &argv) != MPI_SUCCESS) {
        fputs("MPI_Init failed\n", stderr);
        return 1;
    }
    if (MPI_Finalize() != MPI_SUCCESS) {
        fputs("MPI_Finalize failed\n", stderr);
        return 1;
    }
    if (!timestamp(&end)) {
        fputs("probe clock failed after MPI_Finalize\n", stderr);
        return 1;
    }
    const double elapsed =
        (double)(end.tv_sec - begin.tv_sec) + (double)(end.tv_nsec - begin.tv_nsec) / 1000000000.0;
    printf("probe init/finalize=%.6f s\n", elapsed);
    return elapsed < 0.0 ? 1 : 0;
}
