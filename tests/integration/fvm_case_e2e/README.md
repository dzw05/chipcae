# FVM case CLI acceptance (test-first)

`fvm_fvm_case_e2e` selects `fvm.conduction` / `fvm.solid` through
`physics[].solver`, without adding CLI arguments. It is registered with labels
`integration;numerical;fvm`, timeout 1200 seconds and skip return code 77.
This revision adds acceptance tests only. The current loader and schema do not
accept FVM selections, so the numerical acceptance is expected to fail.

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

The imported-board chain deliberately exposes two upstream prerequisites:
the mesh setup must pass polygon data to the tensor-grid builder, and the
results writer must emit `volume` for regular hexes. No loader-only relaxation,
fraction averaging, direct HDF5 editing or forbidden-file change bypasses
these dependencies. `copper_fraction` and `conductivity_diag` are deferred
while their `ResultsPartition` members are absent; a compiled capability
probe makes them mandatory automatically when both members become available.
Importer base-material semantics remain an independent integration dependency.

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
RSS for successful solves cannot be reported until the missing feature runs.

## Measured verification (2026-11-20)

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
