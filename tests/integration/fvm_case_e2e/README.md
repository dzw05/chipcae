# FVM case CLI acceptance

`fvm_fvm_case_e2e` selects `fvm.conduction` / `fvm.solid` through
`physics[].solver`, without adding CLI arguments. It is registered with labels
`integration;numerical;fvm`, timeout 1200 seconds and skip return code 77.
The loader and schema accept FVM selections, and the CLI dispatches to the
thermal FVM library. The required regular-hex volume field remains an upstream
results-writer prerequisite. Acceptance assertions are retained without skips
or tolerance changes.

The brief and the explicit task require the thermal-only stage: zero U,
`warpage_pv=0`, `U_mag_max=0`, and both mechanical summary methods and the
level-2 warning exactly `not solved: fvm thermal-only stage`. These zero values
are placeholders, not computed warpage. `T_max` must use `FVM cell centers`.
The ruling's 14:50 revision instead cancels U=0 and requires solved mechanics;
these tests follow the explicit task's thermal-only scope. That discrepancy
needs resolution before the later mechanics card is wired.

The Python test uses temporary synthetic fixtures. CMake generates a C++20
loader probe in its build directory, linked to the real input loader, and
injects executable paths and the configured MPI launcher, rank flag and
pre/post flags. An absent `RunInputs::family` is reported as null and fails an
assertion; the probe does not manufacture a default or modify production code.
Frozen FEM snapshots were captured at baseline `6eca46089f54f5313554522a068195c722130d44`.
They include every existing public input member, nested material tables,
stack, grid, loads and cut options, with only temporary paths normalized.

The acceptance exercises:

- Both physics orders, FVM defaults even without `solver.linear`, Jacobi and
  GAMG solves, exact mixed-family and preconditioner diagnostics, unknown and
  case-sensitive solver names, duplicate physics, temperature-source rules,
  solid-only uniform temperature and FEM-specific stabilization handling.
- Real schema validation, including the required `output.warpage` object.
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
  Robin energy, unsuccessful solves without a published step, and meaningful
  source-free top-Dirichlet/bottom-Robin Wiener bounds using a separate
  half-covered layer written with the public polyset writer. Its explicit
  base/metal materials and exact areas avoid assumptions about imported board
  artwork or matrix semantics; no layer averaging conversion is used. The
  bottom mean surface rise follows `heat_out/(h*area)` for this source-free
  fixture's sole Robin outflow, avoiding corner artifacts in vertex averaging.
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

The imported-board chain originally exposed two upstream prerequisites:
the mesh setup must pass polygon data to the tensor-grid builder, and the
results writer must emit `volume` for regular hexes. No loader-only relaxation,
fraction averaging, direct HDF5 editing or forbidden-file change bypasses
these dependencies. The ordinary prerequisite merge of #198 and #212 supplies
polygon ingestion, corrected importer base materials and the two copper result
members. FVM populates `copper_fraction` and `conductivity_diag`; the compiled
capability probe makes both mandatory. The writer still omits hex `volume`,
which this card cannot fix within its allowed source files.

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

## Preimplementation RED measurements (2026-11-20)

Both bare and numerical Release builds complete. The standalone acceptance
exits 8: 17 test methods, 61 subtest assertion failures, 103.41 seconds.
There are no compilation or Python execution errors. All five FEM snapshots
match existing fields; the new family assertion fails because the member is
absent. FVM cases fail selection/schema, and imported polygon solves fail
with `internal material cuts are not supported`. The decoder/partition checks
pass. Pinned clang-format 23.1.5 and diff whitespace checks are clean.

| 实测 stage | Wall seconds | Peak RSS KiB | Exit |
| --- | ---: | ---: | ---: |
| board import | 0.012671 | 51112 | 0 |
| board preview | 1.549714 | 51112 | 0 |
| original DEMO-GATE FEM | 49.595592 | 1311924 | 0 |
| DEMO-GATE FVM selection attempt | 1.549439 | 54440 | 14 |

The last row measures input rejection; no successful FVM solve, energy or 1%
comparison has occurred. These are local measurements without a performance
gate. The existing F-1130 regression fails at its inner-layer copper-area
expectation before reaching its documented temperature mismatch. The first
solve regression subset has two timeouts and a SIGKILL in its scaling test.
Existing production and regression-test files remain unchanged.

The full numerical suite exits 8 after 1368.67 seconds: 392 tests,
369 passed, 17 skipped, 6 failed. The failures are the expected new
`fvm_fvm_case_e2e`, plus unchanged `solve_column_gmg_fcbga_scaling`,
`f1130_uncut_cli_e2e`, `stack_writer_outline_polyset`,
`import_assembly_cf_chain` and `ecaddiff_import_pair`. The two initial
timeouts pass in this full run. The two assembly failures report duplicate
material IDs; the ECAD comparison disagrees with its existing expected
summary. Bare full CTest has 9 existing failures out of 237; the new test
skips there because numerical executables are not configured. This test-first
revision does not establish green regression or numerical acceptance.

## Implementation verification (2026-11-20, 实测)

An ordinary merge of upstream #198/#212 resolves the polygon mesh/import
prerequisites and supplies copper result members. Both numerical and bare
Release builds pass. The unchanged acceptance runs 17 methods in 124.375 s:
6 assertion failures and 17 errors, CTest exit 8 (124.68 s). Every error is
missing regular-hex `CellData/volume`; two subsequent assertions report the
result sets left incomplete by those errors. Four frozen snapshot cases
(`default`, `sources`, `solid`, `copper`) differ after the prerequisite merge.
The explicit-field `custom` snapshot remains identical. Selection/schema,
negative inputs, nonconvergence, STOP application and repeated-directory
behavior reach their expected outcomes. No assertions are removed or weakened.

A separate diagnostic audit collects missing-volume failures and continues
checking numerical results. It also exits 8; it is not a passing acceptance
replacement. It confirms additive power/q_vol energy (15 W), fixed-layer
reactions (666.666666666666 W), affine cut/EB reconstruction and np=1/2
agreement. The half-covered Wiener fixture's bottom surface rise is
68.521980018447 K, within [68.491588252067, 79.682854311199] K.

| 实测 stage | Wall seconds | Peak RSS KiB | Exit |
| --- | ---: | ---: | ---: |
| unchanged board import | 0.010388 | 55060 | 0 |
| board preview | 1.504969 | 55060 | 0 |
| board FVM np=1 | 1.532473 | 55060 | 0 |
| board FVM np=2 | 1.612725 | 55060 | 0 |
| board FEM/direct | 1.713540 | 62124 | 0 |
| original DEMO-GATE FEM | 47.716326 | 1305304 | 0 |
| permitted DEMO-GATE FVM | 1.613696 | 55316 | 0 |
| DEMO-GATE FVM hex output | 1.614091 | 55316 | 0 |
| DEMO-GATE FVM embedded Robin | 1.587780 | 55316 | 0 |

The board comparison fails the unchanged 1% gate: cell-center FVM Tmax is
321.92657327984955 K, FEM Q2 nodal Tmax is 323.98160735460056 K; relative
rise difference is 0.07955502135584323. Copper fractions and conductivity
diagonals match exactly by GID. Their output vertex maxima are much closer
(323.139202096108 K and 323.1779403036221 K), consistent with the different
sampling locations contributing to the gate failure. The mandated cell-center
summary, mesh and tolerance are retained.

DEMO-GATE's temperature gate passes: FVM 318.94837439789933 K, FEM
318.89820740109059 K, relative rise difference 0.00241789547592937.
It has nonempty cut cells; hex output retains cut-marked parents and EB
triangles, with unchanged EB temperatures. Its energy imbalance is
2.509934519797364e-10. The embedded-Robin variant, at the unchanged default
rtol=1e-8, returns imbalance 9.758564446658027e-9 and fails the required 1e-9
energy gate. The adapter copies case tolerances and the library ledger;
it does not tighten tolerances silently or replace ledger values.

Bare CTest: 247 tests, 9 failures, 72.11 s. The same dependency-disabled
failures recorded above remain. The requested F-1130 integration subset
reproduces its existing inner copper-area expectation failure (1/2 tests
fails); it stops before thermal comparison. Numerical solve/full-suite
results are recorded when the running verification completes.

Supplemental readback also verifies uniform T=350 K with zero U and the
required summary methods, conflicting Dirichlet corner area weighting, and
both copper diagnostics on DEMO cut/hex/embedded outputs. Exact snapshot
comparison isolates upstream changes to the default `fields` list in four
cases and solid-only conductivity parsing (a: 0→2, b: 0→4 W/(m K)); every
other existing field matches. These checks still report missing volumes as
failures. A new input-order regression method (two malformed FEM physics
variants) was added after reproducing return14 instead of the original
missing-reference code12; final verification covers the guarded family scan.

The requested solve subset completes with 90 passes, 10 upstream-disabled
tests and 3 timeouts (`solve_inputs`, `solve_ghost_scan`,
`solve_column_gmg_fcbga_scaling`), 6892.29 s. The first full numerical run has
444 passes, 18 skips, 10 disabled and 9 failures out of 481, 4065.51 s. It
reproduces the FVM/F-1130 failures and two duplicate-material assembly failures,
and also reports an ODB++ timeout, an obsolete dotted material ID in
`ipc_zip_equals_xml`, a subprocess timeout in `solve_inputs`, and failures in
both FCBGA GMG tests. The ghost scan passes in the full run; original
DEMO-GATE and cut-bimetal pass too. All these failures are retained in the log.

The final Release rebuild passes. The added diagnostic-order regression is
GREEN (both malformed FEM cases correctly retain missing-reference code12).
Fresh final acceptance/full-suite results follow in the final verification
record; the requested all-green outcome is not established by these results.

## Final verification (2026-11-20, 实测)

The final guarded binary builds in Release; clang-format 23.1.5 (`-i`, then
`--dry-run --Werror`), Python parsing and diff whitespace checks pass. Fresh
standalone acceptance runs 18 methods in 113.908 s (CTest 114.17 s), with the
same 6 assertion failures and 17 missing-volume errors. The added FEM error-order
regression passes. No existing assertion or frozen snapshot is changed.

Final full numerical CTest: **447 passed, 18 skipped, 10 upstream-disabled,
6 failed out of 481**, 2568.92 s, exit8. Failures:

- `fvm_fvm_case_e2e`: required hex volume and pre-upstream snapshots, as above.
- `ipc_zip_equals_xml`: old dotted material ID `mat.fr4` conflicts with the
  merged importer's ID validation.
- `solve_column_gmg_fcbga_scaling`: fine run returns -9 (SIGKILL); cause unconfirmed.
- `f1130_uncut_cli_e2e`: existing inner copper-area expectation.
- `stack_writer_outline_polyset` and `import_assembly_cf_chain`: existing
  duplicate-material-ID rejection (exit11).

`solve_inputs` passes in 191.40 s, the FCBGA comparison passes in 111.01 s,
and original `demogate_solve` passes in 120.71 s in this final run. Earlier
subset/full timeouts and failures remain recorded above. Bare final counts
are 204 passed, 34 skipped and 9 failed out of 247. Remote CI was not run;
no push was performed. The required all-green acceptance remains blocked.
