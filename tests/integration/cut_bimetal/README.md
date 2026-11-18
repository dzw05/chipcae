# Synthetic cut bilayer acceptance

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
| A, offset square | [0,0,1,1] | (0,0),(.9,0),(.9,.9),(0,.9) | (7,1),(8,2),(16,4), 1; (8,2), 2 |
| B, chamfered octagon | [0,0,1,1] | (.1,0),(.9,0),(1,.1),(1,.9),(.9,1),(.1,1),(0,.9),(0,.1) | (7,1),(8,2),(16,4), 1 |
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
curvature is `kappa_h=-(d+e)`, the mean strain curvature used by the signed
`tools.vv.derive_cards.timoshenko(...,-100)` reference. For small deflections,
`epsilon_x(z)=epsilon_x(0)-z*w_xx` and likewise in y; averaging the two
curvatures gives `-(d+e)`. This matches the existing pipeline tests and retains
sign sensitivity: a plate bending the wrong way fails the comparison.

The original brief's `d+e` convention and N=4 sampling are reconciled here in
response to the confirmed review findings. The mesh sequence is now
(N,M)=(7,1),(8,2),(16,4). N=4 leaves only nine exported top vertices after the
same 0.1 m exclusion; extra z planes do not add top-plane samples. N=7 is the
smallest integer N that meets the 20-point gate for both outlines (A: 25,
B: 36; A has only 16 at N=6). All fit samples still come directly from solver
output. No interpolated or duplicate samples count toward acceptance. All
material inputs, outlines, edge exclusions, fit rank and numerical tolerances
are retained. Coarse curvature remains diagnostic; fit sufficiency is mandatory.

Before solving, regressions check both curvature signs on known quadratics
with rigid translation/tilt and a mixed term, verify that repeated blocks do
not inflate the distinct-point count, and check fit sufficiency for the actual
generated mesh inputs. The real solves independently enforce the same count
and rank gates on exported hex/cut data.

Finest A/B/C curvature error must be <=3%; A/C relative difference <=1%; finest
A/B `abs(d-e)/abs(kappa_ref)` and `abs(f)/abs(kappa_ref)` <=2%. Every A/B/thermal
run must have cut cells; C must have none. MPI differences for kappa_h,
warpage_pv and U_mag_max must be <=1e-8 against finite, nonzero rank-one values.
Warpage PV otherwise remains diagnostic. All summary quantities come from the
manifest summary array. Observed
`log(error_coarse/error_fine)/log(N_fine/N_coarse)` is diagnostic only;
the denominator accounts for the nonuniform refinement ratios.

The thermal run uses steady conduction followed by mechanics, zmin=298.15 K,
and zmax convection with h=1000 and ambient 348.15 K. With
`q=50/(.02/1+.02/10+1/1000)`, every hex/cut point must satisfy
`T=298.15+q*(clip(z,0,.02)+clip(z-.02,0,.02)/10)` within 1e-6 K.
The test prints the worst block, coordinate, measured and reference temperature.

## Review reproduction

The original acceptance (before this fix) completed nine solves but failed the
two coarse fit-count gates (9 < 20) and all three finest curvature gates
(relative errors near 2). The analytic reference was
-0.036363636363636355 /m, while the old displacement-coefficient sum was positive.
The pre-fix evidence is retained in task logs `v-int-cut-bimetal-normal.log`,
`v-int-cut-bimetal-negative.log`, `v-int-cut-bimetal-restored.log` and the fresh
review log `v-int-cut-bimetal-review-red.log`.

New regression checks were run before applying the fixes: both sign checks
and A/B N=4 sampling failed. Applying only the curvature fix left exactly the
two sampling regressions failing. Logs: `v-int-cut-bimetal-review-regression-red.log`
and `v-int-cut-bimetal-review-sampling-red.log`.

## Measured verification

2026-11-18, review fix based on commit `7d7a179`, Release with
MFEM/MPI/HDF5/JSONCONS enabled, core Kokkos OFF and material Kokkos available.
The existing build already supplies the dependency/compiler paths. All commands
use plain tool names with the shared-slot shim first in PATH, followed by the
installed MPI tools. The box environment sets `OMPI_ALLOW_RUN_AS_ROOT=1`,
`OMPI_ALLOW_RUN_AS_ROOT_CONFIRM=1`, `OPENBLAS_NUM_THREADS=1`, `OMP_NUM_THREADS=1`.
Normal, negative and full-suite invocations use fresh TMPDIRs to avoid
shared temporary artifacts.

```bash
build=../build-v-int-cut-bimetal-release
cmake -S . -B "$build" -DCMAKE_BUILD_TYPE=Release
cmake --build "$build" -j2
ctest --test-dir "$build" -R '^cutbim_cut_bimetal$' -V
```

Configure and build exited 0. Normal acceptance exited **0**, 350.18 seconds,
with nine solver exits 0 and nine schema-validated passing result records.
Signed kappa_ref = -0.036363636363636355 /m. 实测:

| Case | kappa_h (/m), 实测 | e_kappa | warpage_pv (m), 实测 | cut cells | hex cells | fit points / rank |
|---|---|---|---|---|---|---|
| A_nx7_ranks1 | -0.036352470752085919 | 0.00030705431763698379 | 0.0087177237678162638 | 26 | 72 | 25 / 6 |
| A_nx8_ranks1 | -0.036354971650007402 | 0.00023827962479620884 | 0.0089070589753208456 | 60 | 196 | 36 / 6 |
| A_nx16_ranks1 | -0.036362069325634461 | 4.3093545052091272e-05 | 0.0088149644873677917 | 232 | 1568 | 121 / 6 |
| B_nx7_ranks1 | -0.036355170104273429 | 0.00023282213248045433 | 0.0074626058229544084 | 8 | 90 | 36 / 6 |
| B_nx8_ranks1 | -0.036356923112518494 | 0.0001846144057411799 | 0.007459629945294438 | 16 | 240 | 49 / 6 |
| B_nx16_ranks1 | -0.036362421255020372 | 3.3415486939535756e-05 | 0.0074517806506299161 | 96 | 1952 | 169 / 6 |
| C_nx16_ranks1 | -0.036363649929271699 | 3.7305497196590831e-07 | 0.0073602739014961074 | 0 | 2048 | 169 / 6 |
| A_nx8_ranks2 | -0.036354971650007402 | 0.00023827962479620884 | 0.0089070589753208456 | 60 | 196 | 36 / 6 |
| thermal_nx8_ranks1 | — | — | 0.0046219708003735292 | 16 | 240 | — |

实测 maximum thermal error = 7.503331289626658e-12 K, cut block at
(0.9000000000000001, 0, 0.02), measured 341.6282608695577 K versus
341.6282608695652 K. Every hex/cut point was checked.
A/C relative curvature difference = 4.3466583808631972e-5; MPI relative
differences for kappa_h, warpage_pv and U_mag_max were all 0.
All isotropy, fit, cut-cell, temperature and consistency gates passed.
Normal output: task log `v-int-cut-bimetal-review-normal.log`;
configure/build output: `v-int-cut-bimetal-review-configure.log` and
`v-int-cut-bimetal-review-build.log`.

The negative run used the same targeted CTest command with only the local
`kappa_ref` expression temporarily multiplied by 1.1 (reference
-0.039999999999999994 /m). It exited **8**, 316.28 seconds, after all nine
solves exited 0 and all nine result records validated. Only the three finest
3% accuracy gates failed: A16=0.090948266859138344,
B16=0.090939468624490566, C16=0.090908751768207383.
Count/rank, isotropy, cut/control, thermal and MPI gates still passed.
The local driver restored the source byte for byte in `finally`; the multiplier
is not committed. Logs: `v-int-cut-bimetal-review-negative.log` and
`v-int-cut-bimetal-review-negative-driver.log`.

After restoring the source, the full Release suite uses:

```bash
ctest --test-dir "$build" --output-on-failure -V -j1
```

Restored acceptance, executed within that full suite, passed in **430.45
seconds**. All nine solver exits were 0 and all nine result records validated
as pass. Every measured curvature, summary, fit coefficient/count/rank, cell
count and thermal error exactly matched the normal run. MPI differences were
again all 0. The three runs' 实测 errors are:

| Case | Normal e_kappa | Reference x1.1 e_kappa | Restored e_kappa |
|---|---|---|---|
| A_nx7_ranks1 | 0.00030705431763698379 | 0.091188231197851882 | 0.00030705431763698379 |
| A_nx8_ranks1 | 0.00023827962479620884 | 0.091125708749814821 | 0.00023827962479620884 |
| A_nx16_ranks1 | 4.3093545052091272e-05 | 0.090948266859138344 | 4.3093545052091272e-05 |
| B_nx7_ranks1 | 0.00023282213248045433 | 0.091120747393164128 | 0.00023282213248045433 |
| B_nx8_ranks1 | 0.0001846144057411799 | 0.091076922187037515 | 0.0001846144057411799 |
| B_nx16_ranks1 | 3.3415486939535756e-05 | 0.090939468624490566 | 3.3415486939535756e-05 |
| C_nx16_ranks1 | 3.7305497196590831e-07 | 0.090908751768207383 | 3.7305497196590831e-07 |
| A_nx8_ranks2 | 0.00023827962479620884 | 0.091125708749814821 | 0.00023827962479620884 |

The fresh full-suite command exited **8**, 2930.72 seconds: 215 tests,
204 passed, 10 expected skips, and one failure, the previously recorded
`solve_ghost_scan` timeout at its unchanged 300-second limit (300.11 seconds).
The restored cut-bimetal acceptance passed; all other runnable tests passed.
No ghost-scan assertion failure was printed before timeout; its matrix file
contained all 2400 complete matrices. Its source, registered timeout and
numerical checks are unchanged. Full output:
`v-int-cut-bimetal-review-all.log`.

The failed-test rerun uses a fresh TMPDIR and `OPENBLAS_NUM_THREADS=2` for the
dense matrix spectrum analysis (`OMP_NUM_THREADS=1`), with the original checks
and timeout:

```bash
OPENBLAS_NUM_THREADS=2 ctest --test-dir "$build" --rerun-failed --output-on-failure -V
```

That two-thread rerun exited **8**: the unchanged test timed out in 300.10
seconds (300.39 seconds total). A second fresh-TMPDIR rerun restored the
original `OPENBLAS_NUM_THREADS=1`, using the same CTest command, and also
exited **8**, timeout 300.12 seconds (300.33 seconds total).
Logs: `v-int-cut-bimetal-review-rerun-failed.log` and
`v-int-cut-bimetal-review-ghost-original.log`.
The required all-green full suite was **not achieved**. The two confirmed
cut-bimetal findings are fixed and its normal/restored acceptance passes;
the pre-existing ghost-scan timeout remains unresolved. No existing test,
timeout, assertion or tolerance was changed to remove this failure.

The final Release build exited 0 (`ninja: no work to do`), recorded in
`v-int-cut-bimetal-review-final-build.log`. clang-format 23.1.5 was verified;
this fix touches only Python/Markdown, leaving no C/C++ inputs for its `-i`
and subsequent `--dry-run --Werror` phases. Scoped whitespace checks passed.

## Exclusions

This synthetic benchmark does not cover STEP or assembly import, cavities or
region level sets, real materials, or distributed scalability. Two ranks check
numerical agreement only. No real customer data or external solver results are used.
