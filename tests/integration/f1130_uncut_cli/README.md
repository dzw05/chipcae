# F-1130 uncut CLI acceptance

This test uses the public synthetic `tests/integration/ecad/data/board_4layer.xml`.
Its five physical layers are F.Cu, In1.Cu, Core, In2.Cu, B.Cu. Layer order,
material IDs and thicknesses are checked against the existing
`tests/integration/import_cli/golden/board_4layer/stack.json` without changing it.
Material properties are illustrative, copied from `test_ipc_uncut_chain.py`;
all generated V&V records use `data_class: synthetic` and `label: 实测`.

```text
board_4layer.xml → chipcae-import → stack.json + stack.polyset + defects.json
stack.json → tools/stack/copper_fraction.py → solve.stack.json + cf.json
solve.stack.json → chipcae-preview → preview.vtkhdf + JSON cell_count
shared case/stack/materials → chipcae-solve directly → ranks1/
same inputs, profile.ranks=2 → mpiexec/mpirun -n 2 → ranks2/
each completed solve → tools/vv/result_summary.py → summary.json
analytic T(z), mesh/QoI/readback/MPI comparisons → result.json → check_file()
```

The actual import arguments, matching `import_cli/cli_test.py`, are:

```sh
chipcae-import tests/integration/ecad/data/board_4layer.xml -o stack.json \
  --polyset stack.polyset --report defects.json --material-map material_map.json
```

`material_map.json` contains `{"FR4":"mat.fr4","copper":"mat.cu"}`. Every
invocation uses a fresh temporary directory. The script prints results before
cleanup; it never adds generated results to the repository. An early chain
failure also writes a failing serial record with empty QoIs and an explanation
that solving did not complete.

The converted copper layers retain `material: mat.cu`, as emitted by the
existing converter. The test does not substitute FR4. Conductivities are
390 W/(m K) for copper and 0.3 W/(m K) for FR4, with series mixing
`k_eff = 1/(f/k_metal + (1-f)/k_matrix)`. The boundary conditions are
298.15 K at zmin and convection to 348.15 K at zmax, h=1000 W/(m² K).
The independent reference is `q=50/(sum(thickness/k_eff)+1/1000)` and
`T(z)=298.15+q*sum(clipped_layer_depth(z)/k_eff)`.

Both preview and solve use nx=4, ny=4, levels=0, max_ratio=100. Acceptance
requires zero cut cells, all hex cell_kind=0, positive hex count equal to the
reported preview count, matching z-extents, finite temperatures with maximum
absolute reference error ≤1e-8 K, positive finite warpage_pv and U_mag_max,
finite T_max, readback warpage relative error ≤1e-12, and ranks=2/1 QoI relative
errors ≤1e-8 using the absolute ranks=1 value as denominator. The profiles
differ only in ranks; shared input bytes are checked after each solve/readback.
Both available solves/readbacks finish before numerical comparisons start.

## Running

Use plain `cmake` and `ctest` commands through the build-slot wrappers supplied
by the execution environment. Keep the wrapper directory first in PATH; allow
queued commands to wait. In a dedicated build, configure Release, tests, ECAD,
JSONCONS, MFEM, MPI, HDF5 and the installed Kokkos material dependency. Supply
dependency locations through CMake options appropriate to the machine:

```sh
cmake -S . -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCHIPCAE_BUILD_TESTS=ON -DCHIPCAE_ENABLE_ECAD=ON \
  -DCHIPCAE_ENABLE_JSONCONS=ON -DCHIPCAE_ENABLE_MFEM=ON \
  -DCHIPCAE_ENABLE_MPI=ON -DCHIPCAE_ENABLE_HDF5=ON -DCHIPCAE_ENABLE_KOKKOS=ON
cmake --build "$build" --target chipcae-import chipcae-preview chipcae-solve -j2
ctest --test-dir "$build" -N -V -R '^f1130_uncut_cli_e2e$'
ctest --test-dir "$build" -V -R '^f1130_uncut_cli_e2e$'
```

CTest always registers the test, labels `integration;numerical;vv`, timeout
1200 seconds and skip code 77. All three executable environment variables are
injected only under the precise ECAD/target/MFEM/MPI/HDF5 condition in the brief.
JSONCONS must additionally be enabled for functional preview/solve backends.
An unset executable variable or missing numpy/h5py returns 77. An executable
that is configured but fails to run is a failure. All nonzero command exits,
including 13, fail with command, exit code, stdout and stderr; timeouts include
captured diagnostics. The first available `mpiexec`, then `mpirun`, is used
without retry. If neither exists, only the MPI run/comparisons are omitted,
with `SKIP ranks=2: launcher not found`; serial acceptance remains mandatory.

## Verification evidence and current blocker

Evidence is recorded against checkout `b7bef7f`; this is a test-first delivery.
The fixture has copper geometry only on F.Cu. Its other copper polygon sets
are empty. The brief nevertheless requires every copper fraction in `(0,1]`.
The assertion is retained; resolving this fixture/requirement conflict belongs
to M1 and the brief owner. No fixture, golden, converter, or product behavior
is changed here. The strict fraction assertion is collected as a failure while
the unchanged stack continues downstream. Overall acceptance still requires a
resolution from M1 and the brief owner; no zero fraction is accepted by this test.

The Release CLI build exited 0. CTest registration showed the required labels,
1200-second timeout, skip code 77, and all executable paths. The normal
acceptance command was:

```sh
ctest --test-dir "$build" -V -R '^f1130_uncut_cli_e2e$'
```

It exited 8 (script exit 1). 实测: all five imported layers matched the golden;
import took 0.414159 s, fraction conversion 0.112042 s, preview 1.604473 s,
and the chain took 2.133184 s. The four measured fractions were F.Cu=0.012313399999999997,
In1.Cu=0, In2.Cu=0, B.Cu=0. The assertion reported:

```text
M1 copper fraction In1.Cu: actual=0.0 expected 0 < fraction <= 1
```

The printed serial failure record passed `check_file() == []`, with
`solve/readback completed: False`, null temperature error, and no invented
QoIs. Import and conversion exited 0. Continuing after the recorded fraction
failure reached preview, which exited 1 and reported:

```text
$/layers/2/material: homogenized material must end with _eq
```

The imported golden uses `kind: Homogenized`, `material: mat.fr4` for Core;
preview's stack loader rejects this ID. This is a second M1/M2 integration
incompatibility. Neither material IDs nor layer kinds are rewritten by the
test. Solving, readback and numerical comparisons remain blocked, with no
compilation or missing-symbol failure. They are not reported as passing.

The three final experiment commands were:

```sh
# Normal reference
ctest --test-dir "$build" -V -R '^f1130_uncut_cli_e2e$'
# Local reference heat_flux multiplied by 1.01
ctest --test-dir "$build" -V -R '^f1130_uncut_cli_e2e$'
# Original script restored byte for byte
ctest --test-dir "$build" -V -R '^f1130_uncut_cli_e2e$'
```

| Run | CTest exit | 实测 import / fraction / preview / total (s) | Max T error |
| --- | --- | --- | --- |
| Normal | 8 | 0.414159 / 0.112042 / 1.604473 / 2.133184 | Unmeasured |
| Reference ×1.01 | 8 | 0.413680 / 0.072648 / 1.531766 / 2.019468 | Unmeasured |
| Restored | 8 | 0.410094 / 0.060566 / 1.535927 / 2.008661 | Unmeasured |

All three retain the fraction failure and then fail on preview's `_eq`
requirement. No solve completed, so the intended temperature-error mutation
proof and restored green run are **blocked**, rather than verified. The
mutation was removed; it is absent from the committed test. Full serial/MPI
readback agreement and the no-launcher serial-success path also remain
unverified past preview. No acceptance tolerance is loosened.

Boundary checks: removing each executable variable individually returned 77;
controlled ImportError for numpy and h5py separately returned 77. A configured
nonexistent executable returned 1, printed command/exit/stdout/stderr, and
wrote a valid failure record. A real child exiting 13 produced a failure with
both captured streams. With numerical backends disabled, CTest still
registered this test and reported it skipped (CTest exit 0, script exit 77).

The pinned clang-format reports version 23.1.5. This change contains no C/C++
files to format. Python byte compilation and `git diff --check` exited 0.

The complete Release build also exited 0. The full suite command was:

```sh
cmake --build "$build" -j2
ctest --test-dir "$build" --output-on-failure -j2
```

实测: 232 tests in 293.01 s, with 193 passed, 8 skipped and 31 failed (CTest
exit 8). This includes the new acceptance failure, 28 existing tests reporting
uninitialized Kokkos OpenMP, the existing `chain_ipc_uncut_chain` assertion
expecting `mat.fr4` where the converter preserves `mat.cu`, and `event_log`
reporting excessive salvage files. An isolated TMPDIR rerun of `event_log`
passed (1/1, 0.80 s). All failed test names and full output are retained in the
external task evidence log. These existing tests and production code are
unchanged; this delivery does not claim an all-green suite.

## Coverage limits

This card excludes ODB++, cut-cell solving, desktop I3 and CalculiX reference
solutions. Wall times are printed per stage and stored as 实测 in result notes;
this card sets no performance gate. The existing 02:30 self-hosted nightly
workflow owns numerical execution; this change does not edit CI files.
Further observed discrepancies belong to M1 (import), M2 (preview), M3
(solve), or M5 (readback). In particular, the manifest may use all top Q2
nodes while readback fits exported vertices; the strict 1e-12 warpage check
requires a numerical run and is not relaxed here.
