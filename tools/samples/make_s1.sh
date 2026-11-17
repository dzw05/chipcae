#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
prototype=${P1_ROOT:-/workspace/prototypes/p1-mfem-thinlayer}
build=${O1_BUILD:-/workspace/build-o1}
run=${S1_RUN:-/workspace/samples/s1}
np=${NP:-1}
[[ "$np" == 1 || "$np" == 2 ]]
[[ ! -e "$run" ]]
# The original prototype is read-only. Patch only a disposable source copy.
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir "$work/src"
cp -R "$prototype/src/." "$work/src/"
cp "$prototype/CMakeLists.txt" "$work/"
patch -d "$work" -p1 < "$repo/tools/samples/p1-export.patch"
cmake -S "$work" -B "$work/build" -DMFEM_DIR="$prototype/deps/mfem/lib/cmake/mfem"
cmake --build "$work/build" --target p1_thinlayer -j 4
mpirun -np "$np" "$work/build/p1_thinlayer" -o 2 -n 8 -nz 1 -s trilayer -r 10 -dT -100 -sv elast -part 2 -tol 1e-10 -mi 5000 -export "$work/export" > "$work/solver.log"
python3 - "$work/solver.log" <<'PYCODE'
import json,sys
result=json.loads(open(sys.argv[1]).read().splitlines()[-1])
assert result['order']==2 and result['nxy']==8
assert result['solves'][0]['converged'], 'S1 solver did not converge'
assert result['warpage_pv_top'] > 0
PYCODE
# Preserve solver evidence separately.
mkdir -p "$run"
cp "$work/solver.log" "$run/solver.log"
mpirun -np "$np" "$build/io/chipcae_write_results" "$run" "$work/export.stack.json" "$work/export"
