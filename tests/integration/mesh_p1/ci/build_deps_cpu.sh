#!/usr/bin/env bash
# CPU-only, local prefix. Requires cmake, Ninja, C/C++, MPI, curl, tar.
# Versions and URLs come from deps.lock beside this script. The nightly
# workflow caches this prefix; its key hashes deps.lock and this script.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../../.." && pwd)
# shellcheck source=tests/integration/mesh_p1/ci/deps.lock
source "$here/deps.lock"
prefix=$(realpath -m "${1:-$repo/build-k1-deps/prefix}")
case "$prefix/" in "$repo/"*) ;; *) echo 'Dependency prefix must be inside this worktree' >&2; exit 2;; esac
work="$repo/build-k1-deps"
mkdir -p "$work/src" "$prefix"
export CMAKE_PREFIX_PATH="$prefix${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"
export CC=mpicc CXX=mpicxx
jobs=${CMAKE_BUILD_PARALLEL_LEVEL:-2}
# ECAD dependencies use the matching version pins in deps.lock and are fetched
# by the project configure via FetchContent, so they remain static and identical
# between PR and nightly builds. Include deps.lock in the local prefix stamp too.
key=$(sha256sum "$0" "$here/deps.lock" | sha256sum | cut -d' ' -f1)
if [[ -f "$prefix/.k1-ready" && $(cat "$prefix/.k1-ready") == "$key" && -f "$prefix/.k1-manifest" ]]; then
  echo "Dependency prefix at $prefix is already complete."
  exit 0
fi
fetch() {
  local name=$1 url=$2
  if [[ ! -d "$work/src/$name" ]]; then
    curl --fail --location --retry 3 "$url" -o "$work/$name.tar.gz"
    mkdir -p "$work/src/$name"
    tar -xzf "$work/$name.tar.gz" -C "$work/src/$name" --strip-components=1
  fi
}
build() {
  local name=$1 source=$2; shift 2
  cmake -S "$source" -B "$work/$name-build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache "$@"
  cmake --build "$work/$name-build" --parallel "$jobs"
  cmake --install "$work/$name-build"
}
fetch metis "$METIS_URL"
build metis "$work/src/metis" -DGKLIB_PATH="$work/src/metis/GKlib" -DSHARED=ON
fetch hypre "$HYPRE_URL"
build hypre "$work/src/hypre/src" -DHYPRE_ENABLE_MPI=ON -DHYPRE_BUILD_TESTS=OFF -DHYPRE_ENABLE_SHARED=ON
fetch hdf5 "$HDF5_URL"
build hdf5 "$work/src/hdf5" -DHDF5_ENABLE_PARALLEL=ON -DBUILD_STATIC_LIBS=OFF \
  -DHDF5_BUILD_TOOLS=OFF -DHDF5_BUILD_HL_LIB=OFF -DHDF5_BUILD_CPP_LIB=OFF \
  -DBUILD_TESTING=OFF -DHDF5_BUILD_EXAMPLES=OFF -DHDF5_ENABLE_Z_LIB_SUPPORT=OFF -DHDF5_ENABLE_SZIP_SUPPORT=OFF
fetch jsoncons "$JSONCONS_URL"
build jsoncons "$work/src/jsoncons" -DJSONCONS_BUILD_TESTS=OFF
fetch mfem "$MFEM_URL"
build mfem "$work/src/mfem" -DMFEM_USE_MPI=ON -DMFEM_USE_METIS=ON \
  -DHYPRE_DIR="$prefix" -DMETIS_DIR="$prefix" -DMFEM_ENABLE_TESTING=OFF \
  -DMFEM_ENABLE_EXAMPLES=OFF -DMFEM_ENABLE_MINIAPPS=OFF -DMFEM_USE_CUDA=OFF
"$here/write_manifest.sh" "$prefix"
printf '%s\n' "$key" > "$prefix/.k1-ready"
