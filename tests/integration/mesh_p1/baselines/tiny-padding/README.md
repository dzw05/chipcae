# Tiny padding digest baseline

Verified on 2026-11-18 by building the unmodified pre-change commit
`aafc877142d5ebe2d16befe80f8c830384beeb49` in a clean detached worktree.
This is a retrospective verification of the old implementation, not evidence
of when the original test author recorded the literal.

`tiny.stack.json` contains the exact input previously embedded in `preview.cpp`
(with a final newline added). Its file SHA-256 is
`94c3f4d3cbae4df796d749c1649d7bbe6941c7736aca3a17813d1ac6d7f71063`.
The acceptance test now copies this fixture so the verified input and test input
stay identical.

`serial.stdout.jsonl` and `mpi.stdout.jsonl` are the captured, unedited stdout
from the pre-change executable, both exiting 0. Both report:

- `nx0=4`, `ny0=3`, `L=1`, `nz=1`, `node_count=126`, `cell_count=48`.
- `padded_cells=2`, confirming the old padding calculation was used.
- `mesh_sha256=31ccb0783121d620d23890e5bab6e87fa6f86cc4cc8bc521ff9138c715ac49a6`.

The fixed SHA expectation in `preview.cpp` remains this literal. Current code
must preserve it while reporting `padded_cells=13`.

## Reproduce

Run from the repository root (paths below describe the verification environment):

```sh
export PATH=/workspace/tasks/shim:$PATH
export OMP_NUM_THREADS=1 OMPI_ALLOW_RUN_AS_ROOT=1 OMPI_ALLOW_RUN_AS_ROOT_CONFIRM=1
git worktree add --detach /workspace/tasks/fix-gmg-m2-baseline aafc877142d5ebe2d16befe80f8c830384beeb49
git -C /workspace/tasks/fix-gmg-m2-baseline status --porcelain
# The status output must be empty.
cmake -S /workspace/tasks/fix-gmg-m2-baseline -B /workspace/tasks/build-fix-gmg-m2-baseline-review -G Ninja -DCMAKE_BUILD_TYPE=Release -DCHIPCAE_ENABLE_JSONCONS=ON -DCHIPCAE_ENABLE_HDF5=ON -DCHIPCAE_ENABLE_MPI=ON -DCHIPCAE_ENABLE_KOKKOS=ON '-DCMAKE_PREFIX_PATH=/workspace/deps/jsoncons;/workspace/hdf5-mpi;/workspace/tools/env;/workspace/deps/kokkos'
cmake --build /workspace/tasks/build-fix-gmg-m2-baseline-review --target chipcae-preview -j2
fixture="$PWD/tests/integration/mesh_p1/baselines/tiny-padding/tiny.stack.json"
preview=/workspace/tasks/build-fix-gmg-m2-baseline-review/preview/chipcae-preview
"$preview" "$fixture" --nx 7 --ny 5 --levels 2 -o /tmp/tiny-baseline-serial > /tmp/tiny-baseline-serial.jsonl
/workspace/tools/env/bin/mpiexec -n 2 "$preview" "$fixture" --nx 7 --ny 5 --levels 2 -o /tmp/tiny-baseline-mpi > /tmp/tiny-baseline-mpi.jsonl
python3 - <<'PY'
import json
from pathlib import Path
for mode in ('serial', 'mpi'):
    events = [json.loads(line) for line in
              Path(f'/tmp/tiny-baseline-{mode}.jsonl').read_text().splitlines()]
    success = [event for event in events if event['stage'] == 'success']
    assert len(success) == 1
    summary = success[0]
    assert summary['mesh_sha256'] == '31ccb0783121d620d23890e5bab6e87fa6f86cc4cc8bc521ff9138c715ac49a6'
    assert summary['padded_cells'] == 2
    assert json.loads(summary['message'])['mesh_sha256'] == summary['mesh_sha256']
    print(mode, summary['mesh_sha256'])
PY
```

Output grid paths vary with `-o`; they do not participate in the mesh digest.

## Cell-budget red phase

The same unmodified pre-change Release executable was run with this fixture and
`--nx 50001 --ny 1 --levels 1 -o /tmp/tiny-budget-red` on 2026-11-18.
`budget.stdout.jsonl`, `budget.stderr.txt`, and `budget.exit-code.txt` capture
its output and exit status. It exits **1**, so the acceptance requirement
`exit_code == 14` evaluates false; stderr identifies `cell_budget`.
The test validates these records before requiring exit 14 from the current CLI.

Reproduce after the build above:

```sh
export OMP_NUM_THREADS=1 OMP_PROC_BIND=false
"$preview" "$fixture" --nx 50001 --ny 1 --levels 1 -o /tmp/tiny-budget-red > /tmp/tiny-budget-red.stdout.jsonl 2> /tmp/tiny-budget-red.stderr.txt
code=$?
test "$code" -eq 1
test "$code" -ne 14
```
