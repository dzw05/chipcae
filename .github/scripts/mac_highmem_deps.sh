#!/bin/bash
# Run from the existing chipcae-env toolchain; never modify that environment.
set -euo pipefail

usage() {
    echo "Usage: $0 (--check|--install) [--prefix <dependency-root>]" >&2
    exit 2
}
mode=
dependency_root="$HOME/chipcae-mac/deps"
prefix_seen=0
while [ "$#" -gt 0 ]; do
    case "$1" in
        --check|--install)
            [ -z "$mode" ] || usage
            mode="$1"; shift ;;
        --prefix)
            [ "$prefix_seen" -eq 0 ] && [ "$#" -ge 2 ] || usage
            [ -n "$2" ] && [[ "$2" != --* ]] || usage
            dependency_root="$2"; prefix_seen=1; shift 2 ;;
        *) usage ;;
    esac
done
[ -n "$mode" ] || usage

# Resolve every destination before any mkdir/download, including existing links.
# commonpath tests directory boundaries, unlike a string-prefix comparison.
paths=$(python3 - "$HOME" "$dependency_root" <<'PY'
import os
import sys
home = os.path.realpath(sys.argv[1])
root = os.path.realpath(os.path.join(home, "chipcae-mac"))
if root != os.path.join(home, "chipcae-mac"):
    sys.exit("outside path: chipcae-mac must not escape through a symlink")
deps = os.path.realpath(sys.argv[2])
paths = [root, deps, os.path.realpath(os.path.join(deps, "mfem")),
         os.path.realpath(os.path.join(deps, "jsoncons")),
         os.path.realpath(os.path.join(root, ".highmem-work"))]
for path in paths[1:]:
    if path == root or os.path.commonpath([root, path]) != root:
        sys.exit("outside prefix/destination: " + path)
# An incomplete existing installation may contain include/lib symlinks too.
# Reject those before CMake can follow them during a repair installation.
visited = set()
for destination in paths[2:4]:
    for directory, subdirs, files in os.walk(destination, followlinks=True):
        canonical = os.path.realpath(directory)
        if canonical in visited:
            subdirs[:] = []
            continue
        visited.add(canonical)
        for name in subdirs + files:
            child = os.path.join(directory, name)
            if os.path.islink(child) and os.path.commonpath([root, os.path.realpath(child)]) != root:
                sys.exit("outside installation path: " + child)
if any("\n" in path or "\r" in path for path in paths):
    sys.exit("invalid newline in prefix path")
print("\n".join(paths))
PY
)
mac_root=$(printf '%s\n' "$paths" | sed -n '1p')
dependency_root=$(printf '%s\n' "$paths" | sed -n '2p')
mfem_prefix=$(printf '%s\n' "$paths" | sed -n '3p')
jsoncons_prefix=$(printf '%s\n' "$paths" | sed -n '4p')
work_root=$(printf '%s\n' "$paths" | sed -n '5p')
env_prefix="$HOME/chipcae-env/env"

# Read installed files, rather than execute package CMake code during --check.
check() {
    python3 - "$env_prefix" "$mfem_prefix" "$jsoncons_prefix" "$1" <<'PY'
import pathlib
import re
import sys
env, mfem, jsoncons = map(pathlib.Path, sys.argv[1:4])
section = sys.argv[4]
missing = []
def report(name, good, hint):
    print(("OK " if good else "MISSING ") + name + ("" if good else ": " + hint))
    if not good:
        missing.append(name)
def executable(name):
    import os
    return os.access(env / "bin" / name, os.X_OK) and (env / "bin" / name).is_file()
def library(name):
    return any(p.is_file() for p in (env / "lib").glob("lib" + name + ".dylib"))
if section in ("all", "environment"):
    hint = "ask the runner owner to repair the existing " + str(env) + " environment"
    for name in ("cmake", "ninja", "mpicxx"):
        report(name, executable(name), hint)
    compilers = executable("clang") and executable("clang++")
    compilers |= any(executable(p.name) and executable(p.name + "++")
                     for p in (env / "bin").glob("*-clang"))
    report("compiler clang/clang++", compilers, hint)
    report("MPI launcher mpiexec/mpirun", executable("mpiexec") or executable("mpirun"), hint)
    report("PETSc headers/library", (env / "include/petsc.h").is_file()
           and (env / "include/petscsys.h").is_file() and library("petsc"), hint)
    for name in ("lapack", "blas"):
        report("lib" + name, library(name), hint)
    for name, header in (("hdf5", "hdf5.h"), ("metis", "metis.h"), ("HYPRE", "HYPRE.h")):
        headers = [env / "include" / header, env / "include" / name / header]
        report(name + " headers/library", any(p.is_file() for p in headers)
               and library(name), hint)
if section in ("all", "mfem"):
    configs = sorted(mfem.glob("lib*/cmake/mfem/MFEMConfig.cmake"))
    text = configs[0].read_text() if configs else ""
    # Strip comments and require affirmative installed CMake settings.
    text = re.sub(r"#[^\n]*", "", text)
    for key in ("MPI", "PETSC"):
        report("MFEM_USE_" + key + "=YES", bool(re.search(
            r'\bset\s*\(\s*MFEM_USE_' + key + r'\s+"?(YES|ON|TRUE|1)"?\s*\)',
            text, re.I)), "run --install with MFEM 4.8 MPI+PETSc; conda MFEM may disable PETSc")
    report("MFEM headers/shared library", (mfem / "include/mfem.hpp").is_file()
           and any(mfem.glob("lib*/libmfem.dylib")), "run --install")
if section in ("all", "jsoncons"):
    configs = list(jsoncons.glob("share/cmake/jsoncons/jsonconsConfig.cmake"))
    configs += list(jsoncons.glob("lib*/cmake/jsoncons/jsonconsConfig.cmake"))
    report("jsoncons CMake configuration", bool(configs)
           and (jsoncons / "include/jsoncons/json.hpp").is_file(), "run --install (1.4.0)")
sys.exit(bool(missing))
PY
}

if [ "$mode" = --check ]; then
    check all
    exit 0
fi
# Aggregate prerequisite diagnostics, but do not download until they all pass.
check environment
mfem_valid=0
jsoncons_valid=0
if check mfem; then mfem_valid=1; fi
if check jsoncons; then jsoncons_valid=1; fi
if [ "$mfem_valid" -eq 1 ] && [ "$jsoncons_valid" -eq 1 ]; then
    echo "Skip MFEM and jsoncons: existing installations are valid"
    check all
    exit 0
fi

mkdir -p "$work_root"
work=$(mktemp -d "$work_root/install.XXXXXX")
# Keep downloads, sources, build trees, and tool temporary/cache files here.
export TMPDIR="$work/tmp"
export XDG_CACHE_HOME="$work/cache"
mkdir -p "$TMPDIR" "$XDG_CACHE_HOME"
export CONDA_PREFIX="$env_prefix"
if [ -x "$env_prefix/bin/clang" ] && [ -x "$env_prefix/bin/clang++" ]; then
    export CC="$env_prefix/bin/clang" CXX="$env_prefix/bin/clang++"
else
    for cc in "$env_prefix"/bin/*-clang; do
        if [ -x "$cc" ] && [ -x "${cc}++" ]; then
            export CC="$cc" CXX="${cc}++"
            break
        fi
    done
fi
export CMAKE_BUILD_PARALLEL_LEVEL=6
export CTEST_PARALLEL_LEVEL=6
export PETSC_DIR="$env_prefix"
export PETSC_ARCH=""

if [ "$mfem_valid" -eq 1 ]; then
    echo "Skip MFEM: existing installation is valid"
else
    curl -fL --retry 3 https://github.com/mfem/mfem/archive/refs/tags/v4.8.tar.gz \
        -o "$work/mfem-4.8.tar.gz"
    tar -xzf "$work/mfem-4.8.tar.gz" -C "$work"
    cmake -S "$work/mfem-4.8" -B "$work/mfem-build" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$mfem_prefix" \
        -DCMAKE_EXPORT_NO_PACKAGE_REGISTRY=ON \
        -DCMAKE_PREFIX_PATH="$env_prefix" -DBUILD_SHARED_LIBS=ON \
        -DMPI_CXX_COMPILER="$env_prefix/bin/mpicxx" \
        -DMFEM_USE_MPI=YES -DMFEM_USE_PETSC=YES -DMFEM_USE_METIS=YES \
        -DHYPRE_DIR="$env_prefix" -DMETIS_DIR="$env_prefix" \
        -DPETSC_DIR="$env_prefix" -DPETSC_ARCH= \
        -DMFEM_ENABLE_TESTING=OFF -DMFEM_ENABLE_EXAMPLES=OFF \
        -DMFEM_ENABLE_MINIAPPS=OFF -DMFEM_ENABLE_BENCHMARKS=OFF
    # MFEM enables HYPRE as a required dependency of MPI in version 4.8.
    cmake --build "$work/mfem-build" --parallel 6
    cmake --install "$work/mfem-build"
fi
if [ "$jsoncons_valid" -eq 1 ]; then
    echo "Skip jsoncons: existing installation is valid"
else
    curl -fL --retry 3 https://github.com/danielaparker/jsoncons/archive/refs/tags/v1.4.0.tar.gz \
        -o "$work/jsoncons-1.4.0.tar.gz"
    tar -xzf "$work/jsoncons-1.4.0.tar.gz" -C "$work"
    cmake -S "$work/jsoncons-1.4.0" -B "$work/jsoncons-build" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$jsoncons_prefix" \
        -DCMAKE_PREFIX_PATH="$env_prefix" -DJSONCONS_BUILD_TESTS=OFF
    cmake --build "$work/jsoncons-build" --parallel 6
    cmake --install "$work/jsoncons-build"
fi
check all
