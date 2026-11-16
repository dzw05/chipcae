#!/usr/bin/env bash
# Run point-level tests on the GPU (requires a CUDA build; no fabricated results).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${BUILD_DIR:-${ROOT}/build-cuda}"

if ! command -v nvcc >/dev/null 2>&1; then
    echo "ERROR: no CUDA toolkit found (nvcc missing). GPU tests not run." >&2
    echo "T25 / CPU-GPU consistency remain pending until a GPU runner is available." >&2
    exit 2
fi

if [ ! -f "${BUILD}/CTestTestfile.cmake" ]; then
    echo "ERROR: no CUDA build in '${BUILD}'. Run material/scripts/build_cuda.sh first." >&2
    exit 2
fi

ctest --test-dir "${BUILD}" --output-on-failure --output-junit "${BUILD}/gpu_junit.xml"
