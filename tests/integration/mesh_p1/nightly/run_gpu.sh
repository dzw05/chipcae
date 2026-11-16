#!/usr/bin/env bash
# Disabled CI hook. Run on a provisioned CUDA/MFEM machine; never provisions it.
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
build=$(realpath -m "${1:-$repo/build-k1-gpu}")
case "$build/" in "$repo/"*) ;; *) echo 'Build directory must be inside this worktree' >&2; exit 2;; esac
if ! command -v nvidia-smi >/dev/null || ! nvidia-smi -L >/dev/null 2>&1; then
  echo 'SKIP: CUDA device unavailable'; exit 0
fi
if [[ ! -x "$build/tests/integration/mesh_p1/k1_patch" ]]; then
  echo 'SKIP: configure/build with existing MFEM/MPI/Kokkos flags and a CUDA-enabled MFEM prefix first'; exit 0
fi
export CHIPCAE_RUN_GPU=1
ctest --test-dir "$build" -L gpu --output-on-failure --timeout 60 --no-tests=error
