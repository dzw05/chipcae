#!/usr/bin/env bash
# Build the standalone CPU tree (Serial + OpenMP Kokkos).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PREFIX="${KOKKOS_PREFIX:-/workspace/deps/kokkos}"
BUILD="${BUILD_DIR:-${ROOT}/build-cpu}"

if [ ! -f "${PREFIX}/include/Kokkos_Core.hpp" ]; then
    echo "ERROR: Kokkos not found at '${PREFIX}'." >&2
    echo "Set KOKKOS_PREFIX to a Kokkos install (Serial+OpenMP)." >&2
    exit 1
fi

cmake -S "${ROOT}" -B "${BUILD}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="${PREFIX}" \
    -DCHIPCAE_MATERIAL_ENABLE_TESTS=ON \
    "$@"
cmake --build "${BUILD}" -j "${BUILD_JOBS:-$(nproc)}"
