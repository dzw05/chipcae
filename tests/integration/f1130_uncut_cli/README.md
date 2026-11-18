# F-1130 uncut CLI acceptance

CTest registers `f1130_uncut_cli_e2e` through `chipcae_auto_tests`, with labels
`integration;numerical;vv`, timeout 1200 seconds and skip return code 77.
The full import → fractions → preview → solve(np=1/2) → readback → result.json
chain runs when the required numerical executables and Python dependencies are
available. The mandated fixture currently fails a strict fraction check; this
failure remains visible in CTest pending resolution by the owning module.

`f1130_uncut_cli_records` separately runs the 11 assertion/verdict regressions,
including missing-launcher and incomplete-parallel-comparison failures.
Passing those regressions does not establish end-to-end acceptance.

## Inputs and chain

Input: `tests/integration/ecad/data/board_4layer.xml`, a public synthetic 5 mm
square board. Import uses
`tests/integration/import_cli/material_map.json` verbatim:
`{"FR4":"mat.fr4","copper":"mat.cu"}`. These are the existing import CLI
`board_4layer` golden-generation parameters:

```text
chipcae-import <repo>/tests/integration/ecad/data/board_4layer.xml
  -o stack.json --polyset stack.polyset --report defects.json
  --material-map <repo>/tests/integration/import_cli/material_map.json
```

```text
board_4layer.xml + original map → chipcae-import → stack.json + sidecar + defects
stack.json → compare layer names/order, thicknesses and materials with golden
same stack.json → tools/stack/copper_fraction.py → solve.stack.json + cf.json
solve.stack.json → chipcae-preview → preview.vtkhdf + JSON cell_count
shared case/stack/materials → chipcae-solve directly → ranks1/
same inputs, profile.ranks=2 → mpiexec/mpirun -n 2 → ranks2/
each completed solve → tools/vv/result_summary.py → summary.json
analytic T(z), mesh/QoI/readback/MPI checks → result.json → check_file()
```

Each invocation uses fresh temporary directories. Shared solver input bytes
must remain unchanged; profiles differ only in ranks. Commands, measured stage
wall times and result records are printed before cleanup. Records use schema
`chipcae.vv_result/0.2`, `data_class: synthetic` and `label: 实测`. Incomplete
acceptance checks force a failing verdict, even after an early setup failure.
Schema 0.2 permits `reference.value: null`: the reference is a temperature
field, rather than a scalar. `date` records the measurement day in the schema's
YYYYMMDD format; it is intentionally runtime metadata.

## Required comparisons

- Imported layer order, names, literal materials and thicknesses match
  `tests/integration/import_cli/golden/board_4layer/stack.json`; thickness
  relative tolerance is 1e-12.
- All four copper fractions are finite and in `(0,1]`, copied into the converted
  stack without changing layer/material/metal identities. F.Cu independently
  matches its trace area: 0.307835 mm² / 25 mm² = 0.0123134 (relative 1e-12).
- Preview uses `--nx 4 --ny 4 --levels 0 --max-ratio 100`; solve uses exactly
  `{"type":"aligned_cartesian_cut","nx":4,"ny":4,"levels":0,"max_ratio":100}`.
  Solve must have zero cut cells, all hex cell_kind=0, and a positive hex count
  directly equal to preview's cell_count.
- Hex point temperatures match the independent 1D series-resistance reference
  within 1e-8 K. Illustrative material properties and case fields are copied
  from `test_ipc_uncut_chain.py`: copper k=390 W/(m K), FR4 k=0.3 W/(m K),
  zmin=298.15 K, zmax convection h=1000 W/(m² K), T_inf=348.15 K and 3-2-1
  mechanical constraints. For each layer,
  `k_eff=1/(f/k_metal+(1-f)/k_matrix)`,
  `q=50/(sum(thickness/k_eff)+1/1000)` and
  `T(z)=298.15+q*sum(clipped_layer_depth(z)/k_eff)`.
- warpage_pv and U_mag_max are finite and positive; T_max is finite.
  Readback `warpage.w_bfp_m` differs from manifest warpage_pv by at most 1e-12
  relative. MPI QoIs differ from serial by at most 1e-8 relative. Each relative
  error uses the absolute manifest/serial value as its denominator.
- Every result record passes `tools.vv.check_result_data_class.check_file()`.

## Running

Use the build-slot wrappers on PATH, with at most two jobs. Configure Release
with dependency locations supplied through the appropriate CMake options:

```sh
cmake -S . -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCHIPCAE_BUILD_TESTS=ON -DCHIPCAE_ENABLE_ECAD=ON \
  -DCHIPCAE_ENABLE_JSONCONS=ON -DCHIPCAE_ENABLE_MFEM=ON \
  -DCHIPCAE_ENABLE_MPI=ON -DCHIPCAE_ENABLE_HDF5=ON
cmake --build "$build" -j2
ctest --test-dir "$build" -V -R '^f1130_'
ctest --test-dir "$build" --output-on-failure -j2
```

CMake injects `CHIPCAE_IMPORT_EXE`, `CHIPCAE_PREVIEW_EXE` and
`CHIPCAE_SOLVE_EXE` only when ECAD, MFEM, MPI and HDF5 are enabled and all three
executable targets exist. Otherwise the acceptance test remains registered
and returns 77 for missing executable environment. Missing numpy/h5py also
returns 77. Configured commands that fail print their command, status and
stdout/stderr and fail acceptance; each command has a 600 second timeout.

The first available `mpiexec`, then `mpirun`, is used without retry.
If neither exists, acceptance fails and writes a failing result record with
an incomplete parallel check. Both ranks require a successful parallel
comparison before their result record can pass. This follows the current
review requirement, superseding the original brief's launcher-omission rule.

For a direct invocation, supply the executables from the same numerical build:

```sh
CHIPCAE_IMPORT_EXE="$build/kernel/chipcae-import" \
CHIPCAE_PREVIEW_EXE="$build/preview/chipcae-preview" \
CHIPCAE_SOLVE_EXE="$build/apps/chipcae-solve/chipcae-solve" \
  python3 tests/integration/f1130_uncut_cli/test_uncut_cli_e2e.py
```

## Verification (2026-11-19)

The two added regressions failed before the correction: a serial record with
no parallel comparison was accepted, and missing launchers did not cause the
required launcher failure. After the correction, all 11 regressions pass.
CTest discovery verifies the full script command, three executable environment
variables, labels, timeout 1200 and skip return code 77 in the numerical build.
Release configure and build succeed with ECAD, JSONCONS, MFEM, MPI and HDF5
available and optional Kokkos disabled.

`ctest --test-dir "$build" -V -R '^f1130_'` exits 8: the E2E test fails on
In1.Cu=0 after import/conversion, while `f1130_uncut_cli_records` passes all 11
regressions. The same configure and CTest commands in a Release build without
the numerical backends exit 0: E2E skips with code 77, and the records test
passes. No fixture or threshold was changed to obtain these results.

The full `ctest --test-dir "$build" --output-on-failure -j2` run exits 8 after
1277.24 seconds: **232 total, 221 passed, 8 skipped, 3 failed**:

- `f1130_uncut_cli_e2e`: In1.Cu=0, as described below.
- `chain_ipc_uncut_chain`: its unchanged line 53 expects `mat.fr4`, while
  conversion preserves imported `mat.cu` on F.Cu.
- `event_log`: the shared temporary directory contains 10,000 salvage files
  at offset 364, exhausting the event log's salvage-name limit.

An isolated `TMPDIR="$fresh_tmp" ctest --test-dir "$build" -V -j1 -R '^event_log$'`
rerun with a newly created temporary directory exits 0 (1/1 passed, 0.57 seconds).
The original full-suite failure remains recorded above.

`solve_ghost_scan` passes in 251.76 seconds with its unchanged 300 second
timeout. The suite is not green; the mandated input/golden, existing tests and
product sources remain unchanged.

## Fixture and downstream blockers

On the mandated public input, import and fraction conversion succeed, and the
import matches the original golden. 实测 fractions are
F.Cu=0.012313399999999997 and In1.Cu=In2.Cu=B.Cu=0. The strict `(0,1]` assertion
fails on In1.Cu before preview or solving. The failure record passes the
data-class gate. The XML has artwork only on F.Cu; the test preserves the
specified fixture, golden and positive-fraction requirement.

The required normal / reference heat-flux ×1.01 negative / restored passing
experiment cannot be completed on this fixture because it fails before
solving. No passing end-to-end result is claimed.

Earlier diagnostic runs also identified the following downstream conflicts.
They are historical evidence, not stages reached by the current failing chain:

| Blocker | Owner and source | Required resolution |
| --- | --- | --- |
| Three zero copper fractions violate `(0,1]` | M1: [mandated artwork](../ecad/data/board_4layer.xml) and [golden stack](../import_cli/golden/board_4layer/stack.json) | Resolve the input/acceptance contract with all four fractions positive. |
| Core `mat.fr4` violates the homogenized `_eq` suffix | M1/M2/M3: [stack validator](../../../schema/src/stack.cpp) and [golden stack](../import_cli/golden/board_4layer/stack.json) | Agree on a material identity accepted by import, preview and solve. |
| Illustrative `mat.cu`/`mat.fr4` IDs contain forbidden dots | M3/material: [material schema](../../../material/schema/material.v1.schema.json) | Align the specified material IDs with the material-library contract. |
| Preview uses imported minimum 1; solve defaults to minimum 2 | M2/M3: [case loader](../../../apps/chipcae-solve/src/case_loader.cpp) and [golden stack](../import_cli/golden/board_4layer/stack.json) | Identical specified mesh settings must yield identical positive cell counts. |
| Independent readback exceeds relative 1e-12 | M3/M5: [Q2 manifest warpage](../../../apps/chipcae-solve/src/pipeline.cpp) and [vertex readback](../../../tools/vv/result_summary.py) | Align independent sampling/fit contracts while retaining the 1e-12 gate. |

The independent readback field is `warpage.w_bfp_m`;
`warpage.manifest_warpage_pv_m` copies the manifest. The regression rejects a
6.8e-12 discrepancy even if the copied value matches exactly. Temperature,
readback and MPI thresholds remain 1e-8 K, 1e-12 and 1e-8 respectively.

## Coverage limits

This card excludes ODB++, cut-cell solving, desktop I3 and CalculiX reference
solutions. Stage times are measured without a performance gate. The existing
nightly workflow owns numerical execution; no CI files change.
