# FVM case CLI acceptance

`fvm_fvm_case_e2e` selects `fvm.conduction` / `fvm.solid` through
`physics[].solver`, without adding CLI arguments. It is registered with labels
`integration;numerical;fvm`, timeout 1200 seconds and skip return code 77.
The loader and schema accept FVM selections, and the CLI dispatches to the
thermal FVM library. Hex and cut outputs include physical `CellData/volume`.
Acceptance thresholds remain unchanged.

The brief and the explicit task require the thermal-only stage: zero U,
`warpage_pv=0`, `U_mag_max=0`, and both mechanical summary methods and the
level-2 warning exactly `not solved: fvm thermal-only stage`. These zero values
are placeholders, not computed warpage. `T_max` must use `FVM cell centers`.

The Python test uses temporary synthetic fixtures. CMake generates a C++20
loader probe in its build directory, linked to the real input loader, and
injects executable paths and the configured MPI launcher, rank flag and
pre/post flags. An absent `RunInputs::family` is reported as null and fails an
assertion; the probe does not manufacture a default or modify production code.
FEM snapshots are frozen at upstream merge `75878ee` (origin/main `c9c1464`).
Differences from `e181d83` are the upstream #212 default copper diagnostic
fields and solid-only conductivity parsing (a: 2, b: 4 W/(m K)). The explicit
`custom` snapshot is unchanged. Every public input member, nested material
table, stack, grid, load and cut option is compared; only temporary paths
are normalized.

The acceptance exercises:

- Both physics orders, FVM defaults even without `solver.linear`, Jacobi and
  GAMG solves, exact mixed-family and preconditioner diagnostics, unknown and
  case-sensitive solver names, duplicate physics, temperature-source rules,
  solid-only uniform temperature and FEM-specific stabilization handling.
- Real schema validation, including the required `output.warpage` object and
  explicit FVM `volume`. FEM explicitly requesting `volume` retains
  INPUT_INVALID with `invalid output field`.
  Missing jsonschema skips only this check and prints the reason.
- Import of the unchanged public board XML and F-1130 material map, unchanged
  stack/polyset through preview and solve, cell counts and mesh digests,
  readable results, finite T, zero U, required metadata and physical volumes.
- Exactly one rank-0 level-1 `FVM_ENERGY ` record, four unique JSON keys,
  finite nonnegative numbers and an integer iteration count. The test
  independently recomputes imbalance with the brief's formula and tolerances,
  and requires both imbalances at most 1e-9. It validates final residual
  batches, state/exit records and output checksums structurally.
- Zero energy, additive power/q_vol equivalence, fixed-CV reactions, embedded
  Robin energy with explicit `rtol=1e-12`, unsuccessful solves without a
  published step, and meaningful
  source-free top-Dirichlet/bottom-Robin Wiener bounds using a separate
  half-covered layer written with the public polyset writer. Its explicit
  base/metal materials and exact areas avoid assumptions about imported board
  artwork or matrix semantics; no layer averaging conversion is used. The
  bottom mean surface rise follows `heat_out/(h*area)` for this source-free
  fixture's sole Robin outflow, avoiding corner artifacts in vertex averaging.
- The board 1% gate compares independently computed hex `PointData/T` maxima
  using `abs(fvm_vertex_max - fem_vertex_max) / (fem_vertex_max - 298.15)`.
  Both vertex and summary maxima and the absolute-temperature relative
  difference are printed. FVM summary `T_max` remains the cell-center maximum.
- The unchanged DEMO-GATE FEM run and its permitted FVM variant, the 1% Tmax
  rise gate, nonempty cut output, retained embedded triangles with
  `--cut-output hex`, physical cut volume and linear cut-point reconstruction.
  Regular and cut thermal libraries are present at baseline, so DEMO-GATE runs.
- MPI np=1/2 alignment by parent gid and vertex coordinates, with each
  partition's local connectivity interpreted separately; relative T tolerance
  1e-8. STOP tests cover collective agreement, malformed/duplicate/partial
  control lines, one applied acknowledgment and no completed step. Repeated
  run-directory behavior is compared against the real FEM executable.

The decoder's negative fixtures reject missing/duplicate energy records,
duplicate/extra JSON keys, wrong types/levels/ranks, nonfinite values,
inconsistent energy, invalid framing, CRC errors and truncated logs. A
partition fixture verifies that rank ordering is irrelevant and a changed
temperature still fails alignment checks.

FVM always selects `volume`, whether explicitly requested or omitted from
`output.fields`; it is not added to the loader's default list. The writer
filters supplied hex volumes with their cells, validates lengths collectively
and writes the field on every rank, including empty hex partitions. A caller
that supplies no hex volume retains the prior output without that dataset.
Cut serialization, frozen manifest fields and unit metadata are unchanged.
`k1_results_validation` and its MPI variant check input values at rtol 1e-14,
absence when not supplied, and invalid lengths without changing existing cases.
The R1 writer diff is +12/-2 lines (14 changed lines).

The ordinary prerequisite merge of #198/#212 supplies polygon ingestion,
corrected importer base materials and both copper result members. FVM
populates `copper_fraction` and `conductivity_diag`; the compiled capability
probe makes both mandatory. No import artifact is rewritten or averaged.

Use the shared build-slot wrappers on PATH and at most two build jobs:

```sh
cmake -S . -B "$build" -DCMAKE_BUILD_TYPE=Release \
  -DCHIPCAE_BUILD_TESTS=ON -DCHIPCAE_ENABLE_ECAD=ON \
  -DCHIPCAE_ENABLE_JSONCONS=ON -DCHIPCAE_ENABLE_MFEM=ON \
  -DCHIPCAE_ENABLE_MPI=ON -DCHIPCAE_ENABLE_HDF5=ON
cmake --build "$build" -j2
ctest --test-dir "$build" -R '^fvm_fvm_case_e2e$' --output-on-failure
ctest --test-dir "$build" -L integration -R 'f1130|demo_gate' --output-on-failure
ctest --test-dir "$build" -R '^solve_' --output-on-failure
ctest --test-dir "$build" --output-on-failure
```

Supply local dependency prefixes as needed. Missing executables or numpy/h5py
return 77; configured command failures fail acceptance. Where `wait4` is
available, it records each command's measured peak RSS in KiB, alongside wall
time. MPI RSS is the launcher's maximum child RSS, not a sum across ranks.
No timing or RSS value is a numerical acceptance threshold. FVM timing and
RSS for successful solves are reported only after execution.

## Continuation verification (2026-11-20, 实测)

The ordinary merge brings the thermal library to `995424f`. The Release
incremental build and pinned clang-format 23.1.5 (`-i`, then
`--dry-run --Werror`) pass. Before the fix, the added writer regression
fails with missing hex volume, explicit FVM `volume` returns INPUT_INVALID,
and schema validation rejects that selection. The loader probe confirms
exactly the upstream snapshot differences listed above; custom is unchanged.

| 实测 quantity | FVM | FEM |
| --- | ---: | ---: |
| board hex vertex maximum, K | 323.13920209610802 | 323.17794030362211 |
| board summary T_max, K | 321.92657327984955 | 323.98160735460056 |
| DEMO-GATE summary T_max, K | 318.94837439789933 | 318.89820740109059 |

The board temperature-rise relative difference is 0.0015477984622042577
(0.1547798462%, gate 1%). Its absolute-temperature relative difference is
0.00011986649669742984. DEMO-GATE's temperature-rise difference is
0.0024178954759293699 (0.2417895476%, gate 1%). The vertex gate does not
change either summary definition.

Board energy imbalance is 1.3544070916991032e-12 at np=1 (89 iterations)
and 4.4733543527735244e-14 at np=2 (90 iterations). DEMO embedded Robin
at explicit `rtol=1e-12` returns 1.2088229951858545e-13 (445 iterations).
The logged ledger and independent recomputation retain the 1e-9 limit.
The maximum np=1/2 relative temperature difference from `compare_parallel`
is 1.1015562826700222e-14 for board and 1.6240976817373746e-16 for cut;
the unchanged limit is 1e-8.

| 实测 stage | Wall seconds | Peak RSS KiB | Exit |
| --- | ---: | ---: | ---: |
| board import | 0.022801 | 50428 | 0 |
| board preview | 1.533552 | 50428 | 0 |
| board FVM np=1 | 1.533149 | 50428 | 0 |
| board FVM np=2 | 1.866790 | 53044 | 0 |
| board FEM/direct | 1.790768 | 61804 | 0 |
| original DEMO-GATE FEM | 39.224233 | 1311676 | 0 |
| DEMO-GATE FVM | 1.569853 | 55764 | 0 |

Only public synthetic fixtures are used. Remote CI has not been run; no push
or PR is part of this continuation.

Fresh local CTest: `fvm_fvm_case_e2e` passes all 19 methods (97.27 s),
`k1_results_validation*` passes 2/2 (4.24 s), and the existing VTKHDF
regressions pass 5/5 (56.69 s), with no skips or threshold changes.

The additional FEM integration subset passes its record checks but reproduces
the existing `f1130_uncut_cli_e2e` failure: In1.Cu fraction is 0.09 while the
fixture expects zero. It stops before solver/readback. This test and its
fixtures are unchanged; the task's FVM acceptance above passes independently.

The additional `^solve_` subset passes all 93 enabled tests, including the
column GMG scaling regression, with 10 unchanged upstream-disabled tests
(1062.50 s). No FEM source, shared test assertion or disabled-test setting
was changed. The F-1130 fraction check above is the only remaining failure
in the verification performed for this continuation.
