#!/usr/bin/env bash
# CPU-only, local prefix. Requires cmake, Ninja, C/C++, MPI, curl, tar.
# The nightly workflow caches this prefix. The cache key is ImageOS, the
# compiler identity, and the hash of this script (the version pins live here).
# --verify only checks the stamp and required config files/libs, then exits.
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
verify_only=0
prefix_arg=
for arg in "$@"; do
  case "$arg" in
    --verify) verify_only=1 ;;
    *) prefix_arg=$arg ;;
  esac
done
prefix=$(realpath -m "${prefix_arg:-$repo/build-k1-deps/prefix}")
case "$prefix/" in "$repo/"*) ;; *) echo 'Dependency prefix must be inside this worktree' >&2; exit 2;; esac
work="$repo/build-k1-deps"
# Cache key includes this script. Stamp only after every dependency installs.
key=$(sha256sum "$0" | cut -d' ' -f1)

has_lib() {
  local name=$1 f
  for f in "$prefix/lib/lib${name}.so" "$prefix/lib/lib${name}.so."* "$prefix/lib/lib${name}.a"; do
    if [[ -e "$f" ]]; then
      return 0
    fi
  done
  return 1
}

# Cheap integrity check used both as a rebuild skip and as the workflow
# cache-hit gate. On failure the caller should rebuild from scratch.
verify() {
  [[ -f "$prefix/.k1-ready" && $(cat "$prefix/.k1-ready") == "$key" ]] || return 1
  local f
  for f in \
    "$prefix/lib/cmake/mfem/MFEMConfig.cmake" \
    "$prefix/lib/cmake/HYPRE/HYPREConfig.cmake" \
    "$prefix/cmake/hdf5-config.cmake" \
    "$prefix/share/cmake/jsoncons/jsonconsConfig.cmake" \
    "$prefix/include/mfem.hpp" \
    "$prefix/include/jsoncons/jsoncons.hpp"
  do
    [[ -f "$f" ]] || return 1
  done
  has_lib metis && has_lib HYPRE && has_lib hdf5 && has_lib mfem
}

if [[ "$verify_only" == 1 ]]; then
  if verify; then
    echo "Dependency prefix at $prefix passed the integrity check."
    exit 0
  fi
  echo "Dependency prefix at $prefix failed the integrity check." >&2
  exit 1
fi

if verify; then
  echo "Dependency prefix at $prefix is already complete."
  exit 0
fi

mkdir -p "$work/src" "$prefix"
export CMAKE_PREFIX_PATH="$prefix${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"
export CC=mpicc CXX=mpicxx
jobs=${CMAKE_BUILD_PARALLEL_LEVEL:-2}
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
