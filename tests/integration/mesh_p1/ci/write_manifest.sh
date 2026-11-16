#!/usr/bin/env bash
# Write $prefix/.k1-manifest after a complete dependency install.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=tests/integration/mesh_p1/ci/deps.lock
source "$here/deps.lock"
prefix=$(realpath -m "${1:?prefix required}")
manifest="$prefix/.k1-manifest"

compiler_id="$(g++ -dumpmachine)-gcc-$(g++ -dumpfullversion)"
image_os="${ImageOS:-unknown}"
image_version="${ImageVersion:-unknown}"
if command -v mpicxx >/dev/null 2>&1 && mpicxx --showme:version >/dev/null 2>&1; then
  mpi_showme=$(mpicxx --showme:version 2>&1 | tr '\n' ' ')
elif command -v mpicxx >/dev/null 2>&1; then
  mpi_showme=$(mpicxx --version 2>&1 | head -n 1)
else
  mpi_showme=unavailable
fi
system_mpi_actual=unavailable
if command -v dpkg-query >/dev/null 2>&1; then
  system_mpi_actual=$(dpkg-query -W -f='${Version}' "$SYSTEM_MPI_PACKAGE" 2>/dev/null || echo unavailable)
fi

files=(
  lib/libmetis.so
  lib/libHYPRE.a
  lib/libhdf5.so.310.5.1
  lib/libmfem.a
  lib/cmake/mfem/MFEMConfig.cmake
  lib/cmake/HYPRE/HYPREConfig.cmake
  cmake/hdf5-config.cmake
  share/cmake/jsoncons/jsonconsConfig.cmake
)

{
  echo "format=1"
  echo "utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "image_os=$image_os"
  echo "image_version=$image_version"
  echo "compiler_id=$compiler_id"
  echo "mpi_showme=$mpi_showme"
  echo "system_mpi_package=$SYSTEM_MPI_PACKAGE"
  echo "system_mpi_version=$SYSTEM_MPI_VERSION"
  echo "system_mpi_actual=$system_mpi_actual"
  echo "METIS_VERSION=$METIS_VERSION"
  echo "HYPRE_VERSION=$HYPRE_VERSION"
  echo "HDF5_VERSION=$HDF5_VERSION"
  echo "JSONCONS_VERSION=$JSONCONS_VERSION"
  echo "MFEM_VERSION=$MFEM_VERSION"
  echo "deps_lock_sha256=$(sha256sum "$here/deps.lock" | cut -d' ' -f1)"
  echo "build_script_sha256=$(sha256sum "$here/build_deps_cpu.sh" | cut -d' ' -f1)"
  for f in "${files[@]}"; do
    if [[ ! -e "$prefix/$f" ]]; then
      echo "Missing $prefix/$f" >&2
      exit 1
    fi
    echo "SHA256  $(sha256sum "$prefix/$f" | cut -d' ' -f1)  $f"
  done
} > "$manifest"
echo "Wrote $manifest"
