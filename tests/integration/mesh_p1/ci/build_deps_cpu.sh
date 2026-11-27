#!/usr/bin/env bash
# CPU-only, local prefix. Requires CMake, Ninja, C/C++, MPI, OpenBLAS, curl, tar.
# Usage: build_deps_cpu.sh [--verify-only] [prefix]
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../../.." && pwd)
# shellcheck source=tests/integration/mesh_p1/ci/deps.lock
source "$here/deps.lock"
verify_only=false
if [[ ${1-} == --verify-only ]]; then
  verify_only=true
  shift
fi
if [[ $# -gt 1 || ( $# -eq 1 && -z $1 ) || ( $verify_only == true && $# -ne 1 ) ]]; then
  echo "Usage: $0 [--verify-only] [nonempty prefix]" >&2
  exit 2
fi
prefix=$(realpath -m "${1-$repo/build-k1-deps/prefix}")
case "$prefix/" in "$repo/"?*) ;; *) echo 'Dependency prefix must be inside this worktree' >&2; exit 2;; esac
work="$repo/build-k1-deps"
# Rebuilding cleans installed outputs. Never allow a prefix that contains the
# repository scripts or source/build staging directories.
case "$here/" in "$prefix/"*) echo 'Prefix contains dependency scripts' >&2; exit 2;; esac
case "$work/src/" in "$prefix/"*) echo 'Prefix contains dependency sources' >&2; exit 2;; esac
case "$prefix/" in "$work/src/"*|"$work/"*-build/*) echo 'Prefix overlaps dependency staging' >&2; exit 2;; esac
case "$prefix/" in "$repo/.git/"*) echo 'Prefix overlaps Git metadata' >&2; exit 2;; esac
if [[ -e "$repo/.git" ]]; then
  tracked=$(git -C "$repo" ls-files -- "$prefix")
  if [[ -n $tracked ]]; then
    echo 'Dependency prefix must not contain tracked repository files' >&2
    exit 2
  fi
fi
export CMAKE_PREFIX_PATH="$prefix${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"
export PETSC_DIR="$prefix" PETSC_ARCH=''
# Keep this expression identical to verify_prefix.sh: fixed relative filenames
# make the ordered sha256sum records independent of invocation and worktree paths.
# Preserve sha256sum's native separators and newlines in the outer hash input.
key=$(cd "$here" && sha256sum build_deps_cpu.sh deps.lock | sha256sum | cut -d' ' -f1)

validate_manifest() {
  python3 - "$prefix" "$here" "$PETSC_VERSION" "$PETSC_URL" "$PETSC_SHA256" <<'PY_MANIFEST'
import hashlib
from pathlib import Path
import sys

prefix = Path(sys.argv[1]).resolve(strict=True)
here = Path(sys.argv[2])
rows = (prefix / ".k1-manifest").read_text().splitlines()
keys = [row.split("=", 1)[0] for row in rows if "=" in row and not row.startswith("SHA256 ")]
if len(keys) != len(set(keys)):
    raise SystemExit("Duplicate manifest metadata fields")

def field(key):
    values = [row[len(key) + 1:] for row in rows if row.startswith(key + "=")]
    if len(values) != 1 or not values[0].strip():
        raise SystemExit(f"Missing, empty or duplicate manifest field: {key}")
    return values[0]

for name, expected in zip(("PETSC_VERSION", "PETSC_URL", "PETSC_SHA256"), sys.argv[3:]):
    if field(name) != expected:
        raise SystemExit(f"Manifest {name} mismatch")
for name, script in (("manifest_script_sha256", "write_manifest.sh"),
                     ("build_script_sha256", "build_deps_cpu.sh"),
                     ("deps_lock_sha256", "deps.lock")):
    if field(name) != hashlib.sha256((here / script).read_bytes()).hexdigest():
        raise SystemExit(f"Manifest {name} mismatch")
recorded = Path(field("install_prefix")).resolve(strict=True)
relative = Path(field("mfem_config_relpath"))
if recorded != prefix or relative.is_absolute() or ".." in relative.parts:
    raise SystemExit("Invalid manifest prefix or MFEM relative path")
config = (recorded / relative).resolve(strict=True)
if not config.is_file() or not config.is_relative_to(prefix):
    raise SystemExit("Missing or escaped MFEM config")
if config.parts[-len(relative.parts):] != relative.parts or config.parents[len(relative.parts) - 1] != prefix:
    raise SystemExit("MFEM config suffix does not infer the installed prefix")
checksums = {}
for row in rows:
    if not row.startswith("SHA256 "):
        continue
    _, digest, name = row.split(maxsplit=2)
    path = Path(name)
    if path.is_absolute() or ".." in path.parts or name in checksums:
        raise SystemExit(f"Invalid or duplicate checksum path: {name}")
    artifact = (prefix / path).resolve(strict=True)
    if not artifact.is_file() or not artifact.is_relative_to(prefix):
        raise SystemExit(f"Missing or escaped artifact: {name}")
    if hashlib.sha256(artifact.read_bytes()).hexdigest() != digest:
        raise SystemExit(f"Checksum mismatch: {name}")
    checksums[name] = digest
required = {relative.as_posix(), "include/petsc.h", "include/petscsys.h", "include/petscconf.h"}
libraries = [path for directory in ("lib", "lib64")
             for path in (prefix / directory).glob("libpetsc.*") if path.is_file()]
if not libraries:
    raise SystemExit("Missing installed PETSc library")
required.update(path.relative_to(prefix).as_posix() for path in libraries)
if not required.issubset(checksums):
    raise SystemExit(f"Missing artifact checksums: {required - checksums.keys()}")
print(config)
PY_MANIFEST
}

validate_capability() (
  # Generate a fresh consumer on every invocation: cached CMake checks cannot
  # certify a replaced MFEM export or a broken PETSc library.
  local expected_config=$1 smoke_dir
  smoke_dir=$(mktemp -d "${RUNNER_TEMP:-/tmp}/k1-petsc-smoke.XXXXXX") || return
  trap 'rm -rf "$smoke_dir"' EXIT
  cat > "$smoke_dir/CMakeLists.txt" <<'CMAKE' || return
cmake_minimum_required(VERSION 3.20)
project(k1_petsc_smoke LANGUAGES C CXX)
find_package(MFEM CONFIG REQUIRED)
message(STATUS "MFEM_CONFIG=${MFEM_CONFIG}")
message(STATUS "MFEM_USE_PETSC=${MFEM_USE_PETSC}")
file(REAL_PATH "${MFEM_CONFIG}" discovered)
if(NOT discovered STREQUAL K1_EXPECTED_CONFIG)
    message(FATAL_ERROR "MFEM config differs from the installed manifest")
endif()
if(NOT MFEM_USE_PETSC)
    message(FATAL_ERROR "MFEM must export PETSc support")
endif()
add_executable(k1_petsc_smoke main.cpp)
target_link_libraries(k1_petsc_smoke PRIVATE mfem)
CMAKE
  cat > "$smoke_dir/main.cpp" <<'CPP' || return
#include <mfem.hpp>
#include <petsc.h>
int main(int argc, char** argv) {
    static_assert(sizeof(mfem::PetscParMatrix) > 0);
    const auto initialized = PetscInitialize(&argc, &argv, nullptr, nullptr);
    return initialized ? initialized : PetscFinalize();
}
CPP
  cmake -S "$smoke_dir" -B "$smoke_dir/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$prefix" \
    -DMFEM_DIR="$(dirname "$expected_config")" -DK1_EXPECTED_CONFIG="$expected_config" || return
  cmake --build "$smoke_dir/build" --parallel 2
)

verify_prefix() {
  "$here/verify_prefix.sh" "$prefix" || return
  local config
  config=$(validate_manifest) || return
  validate_capability "$config" || return
  echo "PETSc-enabled MFEM prefix verified: $prefix"
}

if [[ $verify_only == true ]]; then
  verify_prefix
  exit 0
fi
if [[ -f "$prefix/.k1-ready" && $(cat "$prefix/.k1-ready") == "$key" ]]; then
  if verify_prefix; then
    echo "Dependency prefix at $prefix is already complete."
    exit 0
  fi
  rm -f "$prefix/.k1-ready"
  echo 'Existing prefix failed validation; discard it before rebuilding.' >&2
  exit 1
fi
# Invalidate success before any configure/build/link operation, then remove old
# installed outputs and CMake caches. Keep versioned source downloads reusable.
rm -f "$prefix/.k1-ready"
rm -rf "$prefix"
mkdir -p "$work/src" "$prefix"
export CC=mpicc CXX=mpicxx
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
  rm -rf "$work/$name-build"
  cmake -S "$source" -B "$work/$name-build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache "$@"
  cmake --build "$work/$name-build" --parallel 2
  cmake --install "$work/$name-build"
}
fetch "metis-$METIS_VERSION" "$METIS_URL"
build metis "$work/src/metis-$METIS_VERSION" -DGKLIB_PATH="$work/src/metis-$METIS_VERSION/GKlib" -DSHARED=ON
fetch "hypre-$HYPRE_VERSION" "$HYPRE_URL"
build hypre "$work/src/hypre-$HYPRE_VERSION/src" -DHYPRE_ENABLE_MPI=ON -DHYPRE_BUILD_TESTS=OFF -DHYPRE_ENABLE_SHARED=ON
fetch "hdf5-$HDF5_VERSION" "$HDF5_URL"
build hdf5 "$work/src/hdf5-$HDF5_VERSION" -DHDF5_ENABLE_PARALLEL=ON -DBUILD_STATIC_LIBS=OFF \
  -DHDF5_BUILD_TOOLS=OFF -DHDF5_BUILD_HL_LIB=OFF -DHDF5_BUILD_CPP_LIB=OFF \
  -DBUILD_TESTING=OFF -DHDF5_BUILD_EXAMPLES=OFF -DHDF5_ENABLE_Z_LIB_SUPPORT=OFF -DHDF5_ENABLE_SZIP_SUPPORT=OFF
fetch "jsoncons-$JSONCONS_VERSION" "$JSONCONS_URL"
build jsoncons "$work/src/jsoncons-$JSONCONS_VERSION" -DJSONCONS_BUILD_TESTS=OFF
# Always verify the official PETSc archive before extraction, including rebuilds.
petsc_source="$work/src/petsc-$PETSC_VERSION"
curl --fail --location --retry 3 "$PETSC_URL" -o "$work/petsc-$PETSC_VERSION.tar.gz"
printf '%s  %s\n' "$PETSC_SHA256" "$work/petsc-$PETSC_VERSION.tar.gz" | sha256sum --check --strict
rm -rf "$petsc_source"
mkdir -p "$petsc_source"
tar -xzf "$work/petsc-$PETSC_VERSION.tar.gz" -C "$petsc_source" --strip-components=1
(
  cd "$petsc_source"
  export PETSC_DIR="$petsc_source"
  python3 ./configure --prefix="$prefix" PETSC_ARCH=arch-ci-cpu \
    --with-cc=mpicc --with-cxx=mpicxx --with-mpi=1 \
    --with-blaslapack-lib=-lopenblas --with-cuda=0 --with-fortran-bindings=0 \
    --with-fc=0 --with-debugging=0 --with-shared-libraries=1 --with-make-np=2
  make -j2 PETSC_DIR="$petsc_source" PETSC_ARCH=arch-ci-cpu all
  make -j2 PETSC_DIR="$petsc_source" PETSC_ARCH=arch-ci-cpu install
)
fetch "mfem-$MFEM_VERSION" "$MFEM_URL"
build mfem "$work/src/mfem-$MFEM_VERSION" -DMFEM_USE_MPI=ON -DMFEM_USE_METIS=ON \
  -DMFEM_USE_PETSC=ON -DPETSC_DIR="$prefix" \
  -DHYPRE_DIR="$prefix" -DMETIS_DIR="$prefix" -DMFEM_ENABLE_TESTING=OFF \
  -DMFEM_ENABLE_EXAMPLES=OFF -DMFEM_ENABLE_MINIAPPS=OFF -DMFEM_USE_CUDA=OFF
# Validate the install before publishing metadata or success. The legacy verifier
# requires a stamp; provide it only after the new consumer has compiled/linked,
# and remove it if subsequent full validation fails.
config=$(find "$prefix" -name MFEMConfig.cmake -type f)
[[ -n "$config" && $config != *$'\n'* ]] || { echo 'Expected one MFEM config' >&2; exit 1; }
validate_capability "$(realpath -e "$config")"
"$here/write_manifest.sh" "$prefix"
trap 'rm -f "$prefix/.k1-ready"' EXIT
printf '%s\n' "$key" > "$prefix/.k1-ready"
verify_prefix
trap - EXIT
