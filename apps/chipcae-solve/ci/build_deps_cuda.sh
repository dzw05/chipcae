#!/usr/bin/env bash
# CUDA MFEM + CPU PETSc/Hypre, isolated from developer dependency installations.
# Usage: build_deps_cuda.sh <prefix> <work-dir>
# Requires system MPI, OpenBLAS, ccache, CMake, Ninja, make, Python and CUDA 12.6
# including cuBLAS/cuSPARSE development packages. Never runs solver/test binaries.
set -euo pipefail
if [[ $# != 2 || -z $1 || -z $2 ]]; then
  echo "Usage: $0 <prefix> <work-dir>" >&2
  exit 2
fi
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../.." && pwd)
# shellcheck source=apps/chipcae-solve/ci/deps_cuda.lock
source "$here/deps_cuda.lock"
# Fail on drift, including empty version/URL/tag fields. Pins are copied, not overridden.
cpu_pins=$(bash -c '
  set -euo pipefail
  source "$1"
  for dependency in METIS HYPRE HDF5 JSONCONS MFEM; do
    for field in VERSION URL; do
      key="${dependency}_${field}"
      printf "%s=%s\n" "$key" "${!key:-}"
    done
  done
  printf "HDF5_TAG=%s\n" "${HDF5_TAG:-}"
' cuda-lock "$repo/tests/integration/mesh_p1/ci/deps.lock")
while IFS='=' read -r key expected; do
  [[ -n $expected && -n ${!key:-} && ${!key} == "$expected" ]] || {
    echo "CPU/CUDA lock mismatch: $key" >&2
    exit 1
  }
done <<< "$cpu_pins"
: "${PETSC_VERSION:?}" "${PETSC_URL:?}" "${PETSC_SHA256:?}"
[[ $PETSC_SHA256 =~ ^[0-9a-f]{64}$ ]]
prefix=$(realpath -m "$1")
work=$(realpath -m "$2")
# Require two distinct directories; do not clean or mutate any existing local dependencies.
if [[ $prefix == / || $work == / || $prefix == "$work" ]]; then
  echo 'Use distinct, dedicated prefix and work directories' >&2
  exit 2
fi
mkdir -p "$prefix" "$work/src" "$work/tmp"
export TMPDIR="$work/tmp"
export CC=mpicc CXX=mpicxx
export CMAKE_PREFIX_PATH="$prefix${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"
export PETSC_DIR="$prefix" PETSC_ARCH=''
# All artifacts, including configure probes, are built under the supplied work directory.
# Native dependency configuration (not solver execution) may run CPU feature probes.
fetch() {
  local name=$1 url=$2 checksum=${3:-}
  curl --fail --location --retry 3 "$url" -o "$work/$name.tar.gz"
  if [[ -n $checksum ]]; then
    printf '%s  %s\n' "$checksum" "$work/$name.tar.gz" | sha256sum --check --strict
  fi
  mkdir -p "$work/src/$name"
  tar -xzf "$work/$name.tar.gz" -C "$work/src/$name" --strip-components=1
}
build() {
  local name=$1 source=$2; shift 2
  cmake -S "$source" -B "$work/$name-build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" \
    -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DCMAKE_C_COMPILER=mpicc -DCMAKE_CXX_COMPILER=mpicxx \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache "$@"
  cmake --build "$work/$name-build" --parallel 2
  cmake --install "$work/$name-build"
}
fetch metis "$METIS_URL"
build metis "$work/src/metis" -DGKLIB_PATH="$work/src/metis/GKlib" -DSHARED=ON
fetch hypre "$HYPRE_URL"
build hypre "$work/src/hypre/src" -DHYPRE_ENABLE_MPI=ON -DHYPRE_BUILD_TESTS=OFF \
  -DHYPRE_ENABLE_SHARED=ON -DHYPRE_ENABLE_CUDA=OFF -DHYPRE_ENABLE_HIP=OFF
fetch hdf5 "$HDF5_URL"
build hdf5 "$work/src/hdf5" -DHDF5_ENABLE_PARALLEL=ON -DBUILD_STATIC_LIBS=OFF \
  -DHDF5_BUILD_TOOLS=OFF -DHDF5_BUILD_HL_LIB=OFF -DHDF5_BUILD_CPP_LIB=OFF \
  -DBUILD_TESTING=OFF -DHDF5_BUILD_EXAMPLES=OFF \
  -DHDF5_ENABLE_Z_LIB_SUPPORT=OFF -DHDF5_ENABLE_SZIP_SUPPORT=OFF
fetch jsoncons "$JSONCONS_URL"
build jsoncons "$work/src/jsoncons" -DJSONCONS_BUILD_TESTS=OFF
fetch petsc "$PETSC_URL" "$PETSC_SHA256"
(
  cd "$work/src/petsc"
  export PETSC_DIR="$work/src/petsc"
  python3 ./configure --prefix="$prefix" PETSC_ARCH=arch-ci-cpu \
    --with-cc=mpicc --with-cxx=mpicxx --with-mpi=1 \
    --with-blaslapack-lib=-lopenblas --with-cuda=0 --with-fortran-bindings=0 \
    --with-fc=0 --with-debugging=0 --with-shared-libraries=1 --with-make-np=2
  make -j2 PETSC_DIR="$work/src/petsc" PETSC_ARCH=arch-ci-cpu all
  make -j2 PETSC_DIR="$work/src/petsc" PETSC_ARCH=arch-ci-cpu install
)
fetch mfem "$MFEM_URL"
build mfem "$work/src/mfem" -DBUILD_SHARED_LIBS=ON \
  -DMFEM_USE_MPI=ON -DMFEM_USE_METIS=ON -DMFEM_USE_PETSC=ON \
  -DHYPRE_DIR="$prefix" -DMETIS_DIR="$prefix" -DPETSC_DIR="$prefix" \
  -DMFEM_USE_CUDA=ON -DCMAKE_CUDA_ARCHITECTURES=80 \
  -DCMAKE_CUDA_COMPILER=/usr/local/cuda-12.6/bin/nvcc \
  -DCUDAToolkit_ROOT=/usr/local/cuda-12.6 \
  -DMFEM_ENABLE_TESTING=OFF -DMFEM_ENABLE_EXAMPLES=OFF -DMFEM_ENABLE_MINIAPPS=OFF
# Root CMake finds PETSc before MFEM. Install the original, pinned PETSc finder
# and its helpers only: the full module directory shadows CMake's FindHDF5.
modules="$prefix/share/chipcae/petsc-cmake"
mkdir -p "$modules"
for module in FindPETSc FindPackageMultipass ResolveCompilerPaths; do
  cp "$work/src/mfem/config/cmake/modules/$module.cmake" "$modules/"
done
# Completion stamp is written last and validated even on exact cache hits.
(
  cd "$here"
  sha256sum deps_cuda.lock build_deps_cuda.sh | sha256sum | cut -d' ' -f1
) > "$prefix/.cuda-solve-ready"
