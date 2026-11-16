#!/usr/bin/env bash
# Verify a restored or just-built dependency prefix, then do a find_package link self-test.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=tests/integration/mesh_p1/ci/deps.lock
source "$here/deps.lock"
prefix=$(realpath -m "${1:?prefix required}")
manifest="$prefix/.k1-manifest"

fail() { echo "Prefix verification failed: $*" >&2; exit 1; }

[[ -d "$prefix" ]] || fail "prefix does not exist: $prefix"
[[ -f "$manifest" ]] || fail "missing $manifest"
[[ -f "$prefix/.k1-ready" ]] || fail "missing $prefix/.k1-ready"

manifest_val() {
  local key=$1
  local line
  line=$(grep -E "^${key}=" "$manifest" | tail -n 1 || true)
  printf '%s\n' "${line#*=}"
}

compiler_id="$(g++ -dumpmachine)-gcc-$(g++ -dumpfullversion)"
image_os="${ImageOS:-unknown}"
[[ "$(manifest_val compiler_id)" == "$compiler_id" ]] || fail "compiler_id: manifest=$(manifest_val compiler_id) runner=$compiler_id"
[[ "$(manifest_val image_os)" == "$image_os" ]] || fail "image_os: manifest=$(manifest_val image_os) runner=$image_os"
[[ "$(manifest_val METIS_VERSION)" == "$METIS_VERSION" ]] || fail "METIS_VERSION mismatch"
[[ "$(manifest_val HYPRE_VERSION)" == "$HYPRE_VERSION" ]] || fail "HYPRE_VERSION mismatch"
[[ "$(manifest_val HDF5_VERSION)" == "$HDF5_VERSION" ]] || fail "HDF5_VERSION mismatch"
[[ "$(manifest_val JSONCONS_VERSION)" == "$JSONCONS_VERSION" ]] || fail "JSONCONS_VERSION mismatch"
[[ "$(manifest_val MFEM_VERSION)" == "$MFEM_VERSION" ]] || fail "MFEM_VERSION mismatch"
[[ "$(manifest_val system_mpi_package)" == "$SYSTEM_MPI_PACKAGE" ]] || fail "SYSTEM_MPI_PACKAGE mismatch"
[[ "$(manifest_val system_mpi_version)" == "$SYSTEM_MPI_VERSION" ]] || fail "SYSTEM_MPI_VERSION mismatch"
[[ "$(manifest_val deps_lock_sha256)" == "$(sha256sum "$here/deps.lock" | cut -d' ' -f1)" ]] || fail "deps.lock sha256 mismatch"
script_hash=$(sha256sum "$here/build_deps_cpu.sh" | cut -d' ' -f1)
[[ "$(manifest_val build_script_sha256)" == "$script_hash" ]] || fail "build_deps_cpu.sh sha256 mismatch"
[[ "$(cat "$prefix/.k1-ready")" == "$script_hash" ]] || fail ".k1-ready stamp does not match the build script"

if command -v dpkg-query >/dev/null 2>&1; then
  actual=$(dpkg-query -W -f='${Version}' "$SYSTEM_MPI_PACKAGE")
  [[ "$actual" == "$SYSTEM_MPI_VERSION" ]] || fail "system MPI is $actual, lock pins $SYSTEM_MPI_VERSION"
fi

while read -r kind hash path; do
  [[ "$kind" == SHA256 ]] || continue
  [[ -e "$prefix/$path" ]] || fail "listed file missing: $path"
  actual=$(sha256sum "$prefix/$path" | cut -d' ' -f1)
  [[ "$actual" == "$hash" ]] || fail "sha256 mismatch for $path"
done < "$manifest"

smoke_dir="${RUNNER_TEMP:-/tmp}/k1-smoke-$$"
rm -rf "$smoke_dir"
cmake -S "$here/smoke_link" -B "$smoke_dir" \
  -DCMAKE_PREFIX_PATH="$prefix" -DCMAKE_BUILD_TYPE=Release
cmake --build "$smoke_dir" --parallel 2
rm -rf "$smoke_dir"
echo "Dependency prefix at $prefix passed the integrity check and link self-test."
