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
curvature follows the V-INT-4 brief's 2026-11-19 review revision; the signed
`tools.vv.derive_cards.timoshenko(...,-100)` reference is unchanged. The brief
records the strain-curvature convention and the coarse-grid correction to N=7.
Its smallest grid provides A:25 and B:36 eligible points, preserving the
20-point gate and 0.1 m exclusion band. No interpolated or duplicate samples
count toward acceptance.

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
The thermal result's `reference.value=0` denotes zero temperature error; the
worst point's analytic temperature is recorded in `qoi.temperature.reference`.

## Verification

2026-11-19, Release with MFEM/MPI/HDF5/JSONCONS enabled. The existing build
supplies the numerical dependencies. Commands below run from the repository
root through the shared-slot shim (at most two jobs), with MPI root permission,
`OPENBLAS_NUM_THREADS=1`, `OMP_NUM_THREADS=1`, and a fresh TMPDIR per invocation.

```bash
build=../build-v-int-cut-bimetal-release
cmake -S . -B "$build" -DCMAKE_BUILD_TYPE=Release
cmake --build "$build" -j2
ctest --test-dir "$build" -R '^cutbim_cut_bimetal$' -V
# Repeat with only kappa_ref temporarily multiplied by 1.1:
ctest --test-dir "$build" -R '^cutbim_cut_bimetal$' -V
# Restore the original source, then run the complete suite:
ctest --test-dir "$build" --output-on-failure -V -j2
ctest --test-dir "$build" --rerun-failed --output-on-failure -V -j1
```

Configure/build exited 0. Normal acceptance exited **0** (318.78 s).
The reference-x1.1 run exited **8** (312.23 s), failing only the
three finest 3% curvature gates after nine successful solves and nine validated
result records. The mutation was protected by `finally` and the source restored
byte for byte. Restored acceptance passed within the full suite. That full run
exited **8** (1486.80 s): **215 tests, 203 passed, 10 expected skips,
two timing failures** (`solve_inputs`, `solve_ghost_scan`). The unchanged,
serial failed-only retry exited **0** (463.34 s), both tests passing.
All 205 runnable tests therefore passed across the full run and retries;
a single all-green full-suite invocation was not obtained. An earlier full
attempt had one MPI-preview JSON parsing abort; its unchanged isolated retry
and its execution in the subsequent full suite both passed.
Normal and restored numerical records match exactly, including all fit
coefficients, manifest quantities and cell counts.

Signed kappa_ref = -0.036363636363636355 /m. All values below are 实测;
e_kappa values are fractions. Normal/restored columns apply to both passing
runs. The thermal row reports maximum absolute temperature error in K.

| Case | kappa_h (/m), normal/restored | e_kappa, normal/restored | e_kappa, ref x1.1 | warpage_pv (m) | cut / hex cells | fit points / rank |
|---|---|---|---|---|---|---|
| A_nx7_ranks1 | -0.0363524707521 | 0.000307054318 | 0.0911882312 | 0.00871772376782 | 26 / 72 | 25 / 6 |
| A_nx8_ranks1 | -0.03635497165 | 0.000238279625 | 0.0911257087 | 0.00890705897532 | 60 / 196 | 36 / 6 |
| A_nx16_ranks1 | -0.0363620693256 | 4.30935451e-05 | 0.0909482669 | 0.00881496448737 | 232 / 1568 | 121 / 6 |
| B_nx7_ranks1 | -0.0363551701043 | 0.000232822132 | 0.0911207474 | 0.00746260582295 | 8 / 90 | 36 / 6 |
| B_nx8_ranks1 | -0.0363569231125 | 0.000184614406 | 0.0910769222 | 0.00745962994529 | 16 / 240 | 49 / 6 |
| B_nx16_ranks1 | -0.036362421255 | 3.34154869e-05 | 0.0909394686 | 0.00745178065063 | 96 / 1952 | 169 / 6 |
| C_nx16_ranks1 | -0.0363636499293 | 3.73054972e-07 | 0.0909087518 | 0.0073602739015 | 0 / 2048 | 169 / 6 |
| A_nx8_ranks2 | -0.03635497165 | 0.000238279625 | 0.0911257087 | 0.00890705897532 | 60 / 196 | 36 / 6 |
| thermal_nx8_ranks1 | — | 7.50333129e-12 K | 7.50333129e-12 K | 0.00462197080037 | 16 / 240 | — |

The thermal maximum is 7.503331289626658e-12 K in the cut block at
(0.9000000000000001, 0, 0.02): measured 341.6282608695577 K,
analytic 341.6282608695652 K. Every hex/cut point was checked. A16/C16
relative curvature difference is 4.3466583808631972e-5; all three MPI
relative differences are zero. Fit, isotropy, cut-cell, thermal and consistency
gates passed without threshold changes. Registration was independently checked
through CTest's JSON output. clang-format 23.1.5 was verified; this change has
no C/C++ files. Scoped whitespace checks passed.

## Exclusions

This synthetic benchmark does not cover STEP or assembly import, cavities or
region level sets, real materials, or distributed scalability. Two ranks check
numerical agreement only. No real customer data or external solver results are used.
