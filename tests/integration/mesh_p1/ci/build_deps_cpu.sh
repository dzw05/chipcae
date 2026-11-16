#!/usr/bin/env bash
# CPU-only, local prefix. Requires cmake, Ninja, C/C++, MPI, curl, tar.
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
prefix=$(realpath -m "${1:-$repo/build-k1-deps/prefix}")
case "$prefix/" in "$repo/"*) ;; *) echo 'Dependency prefix must be inside this worktree' >&2; exit 2;; esac
work="$repo/build-k1-deps"
mkdir -p "$work/src" "$prefix"
export CMAKE_PREFIX_PATH="$prefix${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"
export CC=mpicc CXX=mpicxx
jobs=${CMAKE_BUILD_PARALLEL_LEVEL:-2}
# Cache key includes this script. Stamp only after every dependency installs.
key=$(sha256sum "$0" | cut -d' ' -f1)
if [[ -f "$prefix/.k1-ready" && $(cat "$prefix/.k1-ready") == "$key" ]]; then exit 0; fi
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
fetch metis https://github.com/mfem/tpls/raw/gh-pages/metis-5.1.0.tar.gz
build metis "$work/src/metis" -DGKLIB_PATH="$work/src/metis/GKlib" -DSHARED=ON
fetch hypre https://github.com/hypre-space/hypre/archive/refs/tags/v3.2.1.tar.gz
build hypre "$work/src/hypre/src" -DHYPRE_ENABLE_MPI=ON -DHYPRE_BUILD_TESTS=OFF -DHYPRE_ENABLE_SHARED=ON
fetch hdf5 https://github.com/HDFGroup/hdf5/archive/refs/tags/hdf5_1.14.6.tar.gz
build hdf5 "$work/src/hdf5" -DHDF5_ENABLE_PARALLEL=ON -DBUILD_STATIC_LIBS=OFF \
  -DHDF5_BUILD_TOOLS=OFF -DHDF5_BUILD_HL_LIB=OFF -DHDF5_BUILD_CPP_LIB=OFF \
  -DBUILD_TESTING=OFF -DHDF5_BUILD_EXAMPLES=OFF -DHDF5_ENABLE_Z_LIB_SUPPORT=OFF -DHDF5_ENABLE_SZIP_SUPPORT=OFF
fetch jsoncons https://github.com/danielaparker/jsoncons/archive/refs/tags/v1.4.0.tar.gz
build jsoncons "$work/src/jsoncons" -DJSONCONS_BUILD_TESTS=OFF
fetch mfem https://github.com/mfem/mfem/archive/refs/tags/v4.10.tar.gz
build mfem "$work/src/mfem" -DMFEM_USE_MPI=ON -DMFEM_USE_METIS=ON \
  -DHYPRE_DIR="$prefix" -DMETIS_DIR="$prefix" -DMFEM_ENABLE_TESTING=OFF \
  -DMFEM_ENABLE_EXAMPLES=OFF -DMFEM_ENABLE_MINIAPPS=OFF -DMFEM_USE_CUDA=OFF
printf '%s\n' "$key" > "$prefix/.k1-ready"
