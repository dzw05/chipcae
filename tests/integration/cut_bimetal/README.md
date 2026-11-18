# Synthetic cut bilayer acceptance (TEST-FIRST)

`cutbim_cut_bimetal` exercises the public solver with generated inputs only.
It has labels `integration;numerical;vv`, timeout 2400 seconds, and skip code 77.
Missing solver environment or numpy/h5py skips the test. Without a numerical
backend CMake still registers it. MPI uses the first available `mpiexec`, then
`mpirun`; only absence of both skips the rank comparison. Launcher failure fails.

All lengths are in metres. Each layer is 0.02 thick, with `n_elem_min=1`.
The bottom/top illustrative thermoelastic materials have E=100/200 GPa,
alpha=5e-6/15e-6 per K, nu=0, density=2000, conductivity=1/10 W/(m K), and
stress-free temperature 400 K. Every run uses the same generated material cards.
Mechanical runs cool uniformly from 400 to 300 K with exactly
`constraints: {"rigid":"3-2-1"}`, direct solution, and natural free edges.
The six constrained DOFs are checked in events.bin without prescribing the
uncut test's anchor coordinates.

| Geometry | Background box | Closed polygon vertices | (N,M), ranks |
|---|---|---|---|
| A, offset square | [0,0,1,1] | (0,0),(.9,0),(.9,.9),(0,.9) | (4,1),(8,2),(16,4), 1; (8,2), 2 |
| B, chamfered octagon | [0,0,1,1] | (.1,0),(.9,0),(1,.1),(1,.9),(.9,1),(.1,1),(0,.9),(0,.1) | (4,1),(8,2),(16,4), 1 |
| C, uncut control | [0,0,.9,.9] | (0,0),(.9,0),(.9,.9),(0,.9) | (16,4), 1 |
| thermal, geometry B | [0,0,1,1] | same as B | (8,2), 1 |

Meshes use `aligned_cartesian_cut`, nx=ny=N, levels=1,
`min_elems_per_layer=M`, max_ratio=10, and ghost penalty with quad_degree=6,
gamma_g=0.1. A touches two background sides; its other two sides cut cells.
Each temporary run directory contains case.json, stack.json, materials.json,
profile.json, solve/, and a schema-validated result.json. Results, including
failed comparisons, are written before aggregate assertion failure. Temporary
directories are cleaned on success and failure; generated results are not committed.

The fit concatenates hex/cut points from all partitions, selects z=0.04 within
1e-9, removes points within 0.1 of any closed polygon segment, and deduplicates
(x,y) lexicographically. It fits `[1,x,y,x²,y²,xy]` by least squares and requires
at least 20 distinct points and rank six on every mechanical run. The signed
curvature is exactly `kappa_h=d+e`; its reference is the signed value from
`tools.vv.derive_cards.timoshenko(...,-100)`. No sign correction or absolute-value
comparison is substituted. Coarse fits remain diagnostic when their count gate
fails, so other solves and comparisons can complete.

Finest A/B/C curvature error must be <=3%; A/C relative difference <=1%; finest
A/B `abs(d-e)/abs(kappa_ref)` and `abs(f)/abs(kappa_ref)` <=2%. Every A/B/thermal
run must have cut cells; C must have none. MPI differences for kappa_h,
warpage_pv and U_mag_max must be <=1e-8 against finite, nonzero rank-one values.
Warpage PV otherwise remains diagnostic. All summary quantities come from the
manifest summary array. Observed log2(error_coarse/error_fine) is diagnostic only.

The thermal run uses steady conduction followed by mechanics, zmin=298.15 K,
and zmax convection with h=1000 and ambient 348.15 K. With
`q=50/(.02/1+.02/10+1/1000)`, every hex/cut point must satisfy
`T=298.15+q*(clip(z,0,.02)+clip(z-.02,0,.02)/10)` within 1e-6 K.
The test prints the worst block, coordinate, measured and reference temperature.

## Measured verification

2026-11-18, baseline ae6a8b9, Release; MFEM/MPI/HDF5/JSONCONS enabled,
core Kokkos backend OFF, material Kokkos dependency available. Use the shared-slot
shim first in PATH, then the installed MPI tools. The box uses
`OMPI_ALLOW_RUN_AS_ROOT=1`, `OMPI_ALLOW_RUN_AS_ROOT_CONFIRM=1`,
`OPENBLAS_NUM_THREADS=1`, and `OMP_NUM_THREADS=1`.
The dedicated build is `../build-v-int-cut-bimetal-release` relative to the source
root (`build` below). All build/test tool invocations use plain command names.

Normal command (exit **8**, 453.64 seconds):

```bash
build=../build-v-int-cut-bimetal-release
ctest --test-dir "$build" -R '^cutbim_cut_bimetal$' -V
```

All nine solver exits were 0 and all nine result.json records passed `check_file`.
Signed kappa_ref = -0.036363636363636355 /m. 实测:

| Case | kappa_h (/m), 实测 | e_kappa | warpage_pv (m), 实测 | cut cells | hex cells | fit points / rank |
|---|---|---|---|---|---|---|
| A_nx4_ranks1 | 0.036344411168334978 | 1.9994713071292118 | 0.0078858951162503899 | 14 | 18 | 9 / 6 |
| A_nx8_ranks1 | 0.036354971650007402 | 1.9997617203752038 | 0.0089070589753208456 | 60 | 196 | 36 / 6 |
| A_nx16_ranks1 | 0.036362069325634461 | 1.9999569064549476 | 0.0088149644873677917 | 232 | 1568 | 121 / 6 |
| B_nx4_ranks1 | 0.036342431047406046 | 1.9994168538036667 | 0.0074863304135882915 | 8 | 24 | 9 / 6 |
| B_nx8_ranks1 | 0.036356923112518494 | 1.9998153855942591 | 0.007459629945294438 | 16 | 240 | 49 / 6 |
| B_nx16_ranks1 | 0.036362421255020372 | 1.9999665845130603 | 0.0074517806506299161 | 96 | 1952 | 169 / 6 |
| C_nx16_ranks1 | 0.036363649929271699 | 2.0000003730549722 | 0.0073602739014961074 | 0 | 2048 | 169 / 6 |
| A_nx8_ranks2 | 0.036354971650007402 | 1.9997617203752038 | 0.0089070589753208456 | 60 | 196 | 36 / 6 |
| thermal_nx8_ranks1 | — | — | 0.0046219708003735292 | 16 | 240 | — |

实测 maximum thermal error = 7.503331289626658e-12 K, in the cut block at
(0.9000000000000001, 0, 0.02); measured 341.6282608695577 K versus
341.6282608695652 K. All hex and cut points were checked.
Cut/control relative curvature difference = 4.3466583808631972e-5.
MPI relative differences for kappa_h, warpage_pv and U_mag_max were each 0.
Isotropy and cut-cell gates passed.

The normal failure output is:

```text
cut_bimetal_A_nx4_ranks1: fit assertion failed: count=9 rank=6 (need >=20 and 6)
cut_bimetal_A_nx16_ranks1: 3% accuracy assertion failed: e_kappa=1.9999569064549476
cut_bimetal_B_nx4_ranks1: fit assertion failed: count=9 rank=6 (need >=20 and 6)
cut_bimetal_B_nx16_ranks1: 3% accuracy assertion failed: e_kappa=1.9999665845130603
cut_bimetal_C_nx16_ranks1: 3% accuracy assertion failed: e_kappa=2.0000003730549722
```

These are post-solve assertion failures. The coarse point count confirms the
brief's sampling conflict: the exported vertices leave only a 3x3 interior grid.
The fitted curvature has the opposite sign to the mandated reference; existing
pipeline tests compare `-(d+e)` to the same convention. This evidence identifies
unmet acceptance requirements and a brief/convention conflict; it does not by
itself establish a physical solver defect. The TEST-FIRST delivery preserves
both gates. Achieving a normal green/restored green run needs an explicit ruling
on these requirements or appropriate output/product work outside this scope.

Full normal output is retained in the task log `v-int-cut-bimetal-normal.log`.

Negative command (same build/environment; exit **8**, 403.79 seconds):

```bash
ctest --test-dir "$build" -R '^cutbim_cut_bimetal$' -V
```

Only the working copy's kappa_ref expression was temporarily multiplied by 1.1,
yielding -0.039999999999999994 /m. All nine solves exited 0, all nine records
validated, and measured curvatures, summaries and fit coefficients were identical
to the normal table. The actual finest-grid failures were:

```text
cut_bimetal_A_nx16_ranks1: 3% accuracy assertion failed: e_kappa=1.9090517331408616
cut_bimetal_B_nx16_ranks1: 3% accuracy assertion failed: e_kappa=1.9090605313755094
cut_bimetal_C_nx16_ranks1: 3% accuracy assertion failed: e_kappa=1.9090912482317925
```

Both coarse point-count failures also remained. The negative run reached and
failed the required 3% comparisons after completing every solve; since normal
acceptance was already red, it cannot demonstrate an isolated green-to-red
sensitivity transition. The source was restored byte for byte in a `finally`
block; the temporary multiplier is not committed. The concurrent full-suite
invocation had already loaded the unchanged module before this local mutation.
Full negative output: `v-int-cut-bimetal-negative.log` in the task logs.

Restored command (same build/environment; exit **8**, 356.04 seconds):

```bash
ctest --test-dir "$build" -R '^cutbim_cut_bimetal$' -V
```

The original signed reference was restored to -0.036363636363636355 /m.
All nine solver exits were 0 and all nine records validated. Every measured
curvature, error, summary, cell count, fit count/rank and coefficient exactly
matched the normal table. Thermal error, cut/control difference and the three
zero MPI differences also matched. The same two point-count and three 3%
assertions failed. Full restored output: `v-int-cut-bimetal-restored.log`.

The three runs' 实测 curvature errors (coarse errors are diagnostic) were:

| Case | Normal e_kappa | Reference x1.1 e_kappa | Restored e_kappa |
|---|---|---|---|
| A_nx4_ranks1 | 1.9994713071292118 | 1.9086102792083746 | 1.9994713071292118 |
| A_nx8_ranks1 | 1.9997617203752038 | 1.9088742912501853 | 1.9997617203752038 |
| A_nx16_ranks1 | 1.9999569064549476 | 1.9090517331408616 | 1.9999569064549476 |
| B_nx4_ranks1 | 1.9994168538036667 | 1.9085607761851513 | 1.9994168538036667 |
| B_nx8_ranks1 | 1.9998153855942591 | 1.9089230778129624 | 1.9998153855942591 |
| B_nx16_ranks1 | 1.9999665845130603 | 1.9090605313755094 | 1.9999665845130603 |
| C_nx16_ranks1 | 2.0000003730549722 | 1.9090912482317925 | 2.0000003730549722 |
| A_nx8_ranks2 | 1.9997617203752038 | 1.9088742912501853 | 1.9997617203752038 |

To reproduce the local negative/restored check from the source root, after the
normal command and with the same numerical environment:

```bash
python3 - <<'PY'
from pathlib import Path
import subprocess

path = Path("tests/integration/cut_bimetal/test_cut_bimetal.py")
original = path.read_bytes()
expression = b"kappa_ref = timoshenko(100e9, .02, 5e-6, 200e9, .02, 15e-6, -100)"
assert original.count(expression) == 1
build = Path("../build-v-int-cut-bimetal-release")
command = ["ctest", "--test-dir", str(build), "-R", "^cutbim_cut_bimetal$", "-V"]
def run(mode):
    with (build / f"cut-bimetal-{mode}.log").open("w") as log:
        print(f"command={command!r}", file=log, flush=True)
        result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT)
        print(f"exit={result.returncode}", file=log)
    print(mode, result.returncode)
try:
    path.write_bytes(original.replace(expression, expression + b" * 1.1"))
    run("negative")
finally:
    path.write_bytes(original)
run("restored")
assert path.read_bytes() == original
PY
```

## Release suite and skips

`cmake --build "$build" -j2` exited 0. The complete command
`ctest --test-dir "$build" --output-on-failure -j2` exited 8 in 1390.85 seconds:
215 tests, 202 passed, 10 skipped, 3 failed. The failures were the intended
`cutbim_cut_bimetal` acceptance and existing `event_log` and
`k1_preview_mpi_surface`. The full-suite cut-bimetal invocation completed all
nine solves and produced the same five assertion failures as the normal run.

`event_log` reported exhausted salvage filenames in the shared temporary
directory (10,000 existing artifacts); an isolated-TMPDIR rerun with
`ctest --test-dir "$build" -R '^event_log$' -V` passed, exit 0.
`k1_preview_mpi_surface` reported a JSON parse error in MPI stdout; a targeted
fresh-TMPDIR rerun with
`ctest --test-dir "$build" -R '^k1_preview_mpi_surface$' -V` also failed, exit 8.
That unresolved existing failure is outside the four-file scope. Existing tests
and production code were left untouched. Logs: `v-int-cut-bimetal-all.log`,
`v-int-cut-bimetal-event-log-isolated.log`, and
`v-int-cut-bimetal-preview-isolated.log` in the task logs.

A backend-free configuration still registers the test with the same labels,
timeout and skip code, omitting the solver environment. Its targeted CTest
invocation exited 0 with one expected Skipped result (script exit 77).
Direct missing-solver and missing-Python-dependency checks both returned 77.
A completely bare repository configure encounters the existing
`cte_contract: chipcae::material target missing` guard; providing the local
material dependency allowed the backend-free registration/skip check.

Python syntax and staged whitespace checks passed. clang-format 23.1.5 was
verified; there are no touched C/C++ files for its `-i` and
`--dry-run --Werror` phases. Only these four test files are committed.

## Implementation follow-up: unresolved acceptance contracts

The follow-up to acceptance commit `4c363b5` inspected the complete brief and
the existing output implementation before considering changes. The four allowed
files already implement the requested harness. There is no permitted numerical
implementation change that resolves its five failing assertions while preserving
the specified inputs, exported samples, and signed comparison.

For N=4, the original Cartesian coordinates are 0, .25, .5, .75, and 1.
After clipping A or B and removing the 0.1 m boundary band, the only eligible
top vertices are the nine pairs in `{.25,.5,.75} x {.25,.5,.75}`. New vertices
introduced by clipping lie on polygon edges and are excluded. Changing M only
adds z planes; it adds no top-plane x/y samples. Hex output uses mesh vertices
(`apps/chipcae-solve/src/pipeline.cpp`, the `GetVertexDofs` output loop), and
cut output samples the polyhedron vertices
(`apps/chipcae-solve/src/cut_output.hpp`, `append_cut_output`). Counting repeated
vertices, synthesizing samples, or changing N would violate the required fit.
This is a sampling/output contract decision for M5 and the brief owner.

The sign discrepancy also occurs for uncut control C, so it cannot be attributed
to cut integration alone. Existing `tests/solve/test_pipeline.py` and
`tests/solve/test_cut_pipeline.py` explicitly negate the quadratic coefficient
sum when comparing to their analytic convention. This brief explicitly requires
the positive sum `d+e` against the signed `timoshenko(...,-100)` reference.
Changing either sign in this test would change its acceptance contract. The
brief owner must reconcile the convention before a physical solver defect can
be assigned to M3.

The brief's prohibition on product/previous-test/tool changes and its instruction
to keep failing tests red apply to this follow-up. All assertions, mesh inputs,
material cards, formulas, and tolerances remain unchanged. Normal/restored green
runs, an isolated green-to-red negative check, and an all-green CTest suite
cannot be delivered under the current requirements. The previous three-run
logs were inspected again: each completed nine solves and validated nine result
records, and the negative run reached all three required 3% comparisons. No
new negative reference mutation was needed for this documentation-only follow-up.

The follow-up Release configure and `cmake --build "$build" -j2` both exited 0.
The registered labels, timeout, skip code and solver environment were checked
again. Backend-free CTest skipped as expected, and direct missing-solver and
missing-Python-dependency checks both returned 77. Python syntax passed;
clang-format 23.1.5 has no changed C/C++ files to format or check.

The fresh full-suite command was
`ctest --test-dir "$build" --output-on-failure -V -j2`, with an isolated temporary
directory supplied through TMPDIR. Its `cutbim_cut_bimetal` entry failed after
380.79 seconds with exactly the five assertions above. All nine solver exits
were 0 and all nine result records validated. Every measured row exactly matched
the original normal run, including the thermal maximum error, A/C difference,
and zero MPI differences. The extracted numerical output is retained in the
task log `v-int-cut-bimetal-impl-numerical.log`.

This fresh full suite exited 8 in 1298.04 seconds: 215 tests, 203 passed,
10 skipped, and two failed. In addition to `cutbim_cut_bimetal`, the unchanged
`solve_ghost_scan` exceeded its existing 300-second timeout (300.01 seconds).
It had passed in 286.74 seconds in the original suite. No cause is asserted
from the timeout alone, and its test/timeout were not changed. The previously
reported `event_log` and `k1_preview_mpi_surface` failures did not reproduce;
both passed. Full output: task log `v-int-cut-bimetal-impl-all.log`.

A subsequent isolated-TMPDIR rerun with
`ctest --test-dir "$build" -R '^solve_ghost_scan$' -V` exited 0; the test passed
in 266.42 seconds (266.54 seconds total). This does not erase the full-suite
timeout or establish its cause. Output:
`v-int-cut-bimetal-impl-ghost-isolated.log` in the task logs.

## Exclusions

This synthetic benchmark does not cover STEP or assembly import, cavities or
region level sets, real materials, or distributed scalability. Two ranks check
numerical agreement only. No real customer data or external solver results are used.
