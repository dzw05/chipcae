#!/usr/bin/env bash
# Run against a provisioned CUDA-enabled MFEM prefix; no dependency provisioning.
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
if ! command -v nvidia-smi >/dev/null || ! nvidia-smi -L | grep -q 'GPU'; then
  echo 'SKIP: no CUDA GPU is available' >&2
  exit 77
fi
prefix=${1:?Usage: run_gpu.sh CUDA_MFEM_PREFIX [BUILD_DIR]}
build=$(realpath -m "${2:-$repo/build-k1-gpu}")
case "$build/" in "$repo/"*) ;; *) echo 'Build directory must be inside this worktree' >&2; exit 2;; esac
cmake -S "$repo" -B "$build" -G Ninja -DCHIPCAE_WARNINGS_AS_ERRORS=ON \
  -DCMAKE_PREFIX_PATH="$prefix${CMAKE_PREFIX_PATH:+;$CMAKE_PREFIX_PATH}" \
  -DCHIPCAE_ENABLE_MPI=ON -DCHIPCAE_ENABLE_MFEM=ON -DCHIPCAE_ENABLE_HYPRE=ON
cmake --build "$build" --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-2}"
test -x "$build/tests/integration/mesh_p1/k1_patch"
test -x "$build/tests/integration/mesh_p1/k1_cut_mfem"
export CHIPCAE_RUN_GPU=1
ctest --test-dir "$build" -L gpu --output-on-failure --timeout 60 --no-tests=error
