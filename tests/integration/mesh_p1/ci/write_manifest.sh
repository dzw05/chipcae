#!/usr/bin/env bash
# Write $prefix/.k1-manifest after a complete dependency install.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=tests/integration/mesh_p1/ci/deps.lock
source "$here/deps.lock"
prefix=$(realpath -e "${1:?prefix required}")
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

# Discover installed artifacts before opening any output. Keep legacy checksums
# (notably the actual static HYPRE build) compatible with verify_prefix.sh.
artifacts=$(python3 - "$prefix" <<'PY_ARTIFACTS'
from pathlib import Path
import sys

prefix = Path(sys.argv[1]).resolve(strict=True)
configs = sorted(prefix.rglob("MFEMConfig.cmake"))
if len(configs) != 1:
    raise SystemExit(f"Expected one installed MFEMConfig.cmake, found {len(configs)}")
headers = [prefix / "include" / name for name in ("petsc.h", "petscsys.h", "petscconf.h")]
headers += sorted((prefix / "include").glob("petsc*.h"))
headers += sorted((prefix / "include/petsc").rglob("*.h"))
libraries = sorted(path for directory in ("lib", "lib64")
                   for path in (prefix / directory).glob("libpetsc.*") if path.is_file())
if not libraries:
    raise SystemExit("Missing installed PETSc library")
paths = [configs[0], *dict.fromkeys(headers), *libraries]
for path in paths:
    if not path.is_file() or not path.resolve(strict=True).is_relative_to(prefix):
        raise SystemExit(f"Missing or escaped installed artifact: {path}")
    relative = path.relative_to(prefix).as_posix()
    if any(char.isspace() for char in relative):
        raise SystemExit(f"Unsupported whitespace in artifact path: {relative}")
    print(relative)
PY_ARTIFACTS
)
mapfile -t installed_files <<< "$artifacts"
files=(
  lib/libmetis.so
  lib/libHYPRE.a
  lib/libhdf5.so.310.5.1
  lib/libmfem.a
  lib/cmake/HYPRE/HYPREConfig.cmake
  cmake/hdf5-config.cmake
  share/cmake/jsoncons/jsonconsConfig.cmake
  "${installed_files[@]}"
)
# A failure must leave the previous manifest intact, including failures while
# hashing. Rename within the prefix so publication is atomic.
temporary=$(mktemp "$prefix/.k1-manifest.XXXXXX")
trap 'rm -f "$temporary"' EXIT

{
  echo "format=1"
  echo "install_prefix=$prefix"
  echo "mfem_config_relpath=${installed_files[0]}"
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
  echo "PETSC_VERSION=$PETSC_VERSION"
  echo "PETSC_URL=$PETSC_URL"
  echo "PETSC_SHA256=$PETSC_SHA256"
  echo "manifest_script_sha256=$(sha256sum "$here/write_manifest.sh" | cut -d' ' -f1)"
  echo "deps_lock_sha256=$(sha256sum "$here/deps.lock" | cut -d' ' -f1)"
  echo "build_script_sha256=$(sha256sum "$here/build_deps_cpu.sh" | cut -d' ' -f1)"
  for f in "${files[@]}"; do
    if [[ ! -e "$prefix/$f" ]]; then
      echo "Missing $prefix/$f" >&2
      exit 1
    fi
    checksum=$(sha256sum "$prefix/$f" | cut -d' ' -f1)
    printf 'SHA256  %s  %s\n' "$checksum" "$f"
  done
} > "$temporary"
mv -f "$temporary" "$manifest"
echo "Wrote $manifest"
