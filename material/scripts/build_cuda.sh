#!/usr/bin/env bash
# Attempt a CUDA build. Fails clearly when no CUDA toolkit / CUDA Kokkos is available.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PREFIX="${KOKKOS_CUDA_PREFIX:-/workspace/deps/kokkos-cuda}"
BUILD="${BUILD_DIR:-${ROOT}/build-cuda}"

if ! command -v nvcc >/dev/null 2>&1; then
    echo "ERROR: no CUDA toolkit found (nvcc missing). CUDA build not attempted." >&2
    echo "This environment has no GPU; T25 stays pending." >&2
    exit 2
fi

if [ ! -f "${PREFIX}/include/Kokkos_Core.hpp" ]; then
    echo "ERROR: Kokkos CUDA build not found at '${PREFIX}'." >&2
    echo "Build Kokkos with Kokkos_ENABLE_CUDA=ON and set KOKKOS_CUDA_PREFIX." >&2
    exit 2
fi

cmake -S "${ROOT}" -B "${BUILD}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="${PREFIX}" \
    -DCHIPCAE_MATERIAL_ENABLE_TESTS=ON \
    -DCHIPCAE_MATERIAL_ENABLE_PYTHON=OFF \
    "$@"
cmake --build "${BUILD}" -j "${BUILD_JOBS:-$(nproc)}"
