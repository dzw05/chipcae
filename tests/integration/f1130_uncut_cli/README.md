# F-1130 uncut CLI diagnostic (not registered as acceptance)

The full chain is a **manual, non-registered diagnostic**, with every required
check active. It cannot establish acceptance on the mandated fixture until the
[contract blockers](#contract-blockers-and-reactivation) are resolved outside
this brief. The review explicitly permits this scope instead of a permanently
failing acceptance CTest. No passing end-to-end result is claimed.

CTest runs only `f1130_uncut_cli_records`, the nine existing assertion/verdict
regression cases. That test does not claim to exercise the full CLI chain.
`f1130_uncut_cli_e2e` is absent from CTest, including numerical builds; it is not
disabled, marked expected-failure or turned into a passing/skip result.

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
ctest --test-dir "$build" -V -R '^f1130_uncut_cli_records$'
ctest --test-dir "$build" --output-on-failure -j2
```

Run the full diagnostic explicitly from the repository root, supplying the
executables from the same numerical build (locations depend on the build):

```sh
CHIPCAE_IMPORT_EXE="$build/kernel/chipcae-import" \
CHIPCAE_PREVIEW_EXE="$build/preview/chipcae-preview" \
CHIPCAE_SOLVE_EXE="$build/apps/chipcae-solve/chipcae-solve" \
  python3 tests/integration/f1130_uncut_cli/test_uncut_cli_e2e.py
```

CTest registers `f1130_uncut_cli_records`, labels `integration;vv`, timeout
1200 seconds, using unittest discovery without executing the diagnostic's
`main()`. It runs all nine existing regression cases. The manual diagnostic
retains exit 77 for missing executable environment or numpy/h5py. A configured
command's failure always fails the diagnostic with its command, exit status and
stdout/stderr. Each command retains its 600 s timeout; the manual chain has no
outer CTest timeout.
The first available `mpiexec`, then `mpirun`, is used without retry. If neither
exists, only ranks=2 and its comparisons are omitted with an explicit message.
Nine regression cases also check input generation, literal material matching,
zero-fraction rejection, result completeness, per-run verdicts and numerical
failures. A readback discrepancy of 6.8e-12 must fail even when the summary's
copied manifest value matches exactly.

## Historical verification and blockers (2026-11-18–19)

Release configure/build succeeded with ECAD, JSONCONS, MFEM, MPI and HDF5
available and optional Kokkos disabled. All nine regression cases pass.

The full `ctest --test-dir "$build" --output-on-failure -j2` run exited 8
after 1795.40 s: **231 total, 220 passed, 8 skipped, 3 failed**:

- `f1130_uncut_cli_e2e`: the zero-fraction failure described below.
- `chain_ipc_uncut_chain`: expects copper-layer base `mat.fr4`, while the
  converted stack preserves imported `mat.cu` (existing test line 53).
- `solve_ghost_scan`: reached its unchanged 300 s timeout.

`event_log` and `k1_preview_mpi_surface` both passed this run. That suite run was
not green; the brief forbids edits to the existing chain test/product code.
An isolated `ctest --test-dir "$build" -V -j1 -R '^solve_ghost_scan$'` rerun
passed in 280.26 s with its original 300 s timeout (CTest exit 0).

Before this correction, the target CTest reproduced the reviewed readback
failure: 实测 manifest warpage_pv=8.118595078913672e-6 m versus readback
8.118595078858198e-6 m, relative difference 6.8329410744551444e-12. CTest exited
8 (script exit 1). That run used the rejected populated fixture and extra mesh
parameter, so it provides diagnostic evidence only.

After restoring the mandated inputs, the same CTest command exits 8 (script
exit 1): import and conversion exit 0, and the import matches the original
golden. 实测 fractions are F.Cu=0.012313399999999997 and
In1.Cu=In2.Cu=B.Cu=0. The strict positive-fraction assertion fails on In1.Cu;
the failing result record passes the data-class gate. The original XML has
artwork only on F.Cu. The script does not substitute another board to proceed.

Separate diagnostics on this exact converted stack, without changing its
contents, also establish that preview exits 1 and solve exits 14:
`$/layers/2/material: homogenized material must end with _eq`.
The golden Core material is `mat.fr4`. Further contract conflicts are visible
in the existing code: material IDs disallow dots in
`material/schema/material.v1.schema.json`, and solve defaults to at least two
elements per layer in `apps/chipcae-solve/src/case_loader.cpp`, whereas the
imported stack minima are one. Downstream numerical acceptance cannot be
established until these input/contract conflicts are resolved.

Readback remains independently blocked: `apps/chipcae-solve/src/pipeline.cpp`
fits Q2 top nodes for manifest warpage, while `tools/vv/result_summary.py`
fits exported vertices. Comparing `manifest_warpage_pv_m` instead would compare
a copied manifest value with its source and lose the independent readback
check. The 1e-12 gate remains unchanged. Both current OAI MUST items and both
current CodeBuddy MUST items are valid; this revision uses their permitted
non-registered diagnostic scope and links the remaining blockers below.

The required reference heat-flux ×1.01 negative experiment and passing restored
run cannot be demonstrated on the mandated fixture: it fails before solving.
Earlier experiments using substitute fixtures do not establish acceptance.

## Diagnostic-scope verification (2026-11-19)

With the same Release build and ECAD, JSONCONS, MFEM, MPI and HDF5 enabled:

- `cmake -S . -B "$build" -DCMAKE_BUILD_TYPE=Release` and
  `cmake --build "$build" -j2` both exit 0.
- `ctest --test-dir "$build" -V -R '^f1130_'` exits 0: only
  `f1130_uncut_cli_records` runs, with all nine regression cases passing.
- `ctest --test-dir "$build" --show-only=json-v1` confirms that the full
  `f1130_uncut_cli_e2e` diagnostic is absent from registration.
- `ctest --test-dir "$build" --output-on-failure -j2` exits 8 after
  1087.93 s: **231 total, 222 passed, 8 skipped, 1 failed**. The sole failure is
  the existing [chain test](../import_solve_chain/test_ipc_uncut_chain.py), line
  53: it expects copper-layer base `mat.fr4` but conversion preserves `mat.cu`.
  That test and the product implementation are outside the allowed file scope;
  the full suite is not green. `solve_ghost_scan` passes in 278.43 s with its
  original 300 s timeout, and the F-1130 records CTest passes in 0.42 s.
- The explicit Python diagnostic command above exits 1 after import and
  conversion. Its nine regressions pass, the original zero-fraction assertion
  fails, and the failing result record passes the data-class gate. No downstream
  acceptance or restored passing run is claimed.
- In-memory mutations allowing zero fractions or raising the readback tolerance
  to 1e-10 each trigger the corresponding existing regression failure. The source
  remains unchanged, with strict positivity and relative 1e-12 readback checks.
- `git diff --check` passes. The pinned clang-format is 23.1.5; this revision
  changes no C/C++ files, so no C/C++ formatting is required.

## Contract blockers and reactivation

These are unresolved dependencies, not waived acceptance checks. Source links
identify the owning contracts so that future work can resolve them without
changing this brief's fixture, golden or thresholds:

| Blocker | Owner and source | Required resolution before acceptance registration |
| --- | --- | --- |
| Three zero copper fractions violate `(0,1]` | M1: [mandated artwork](../ecad/data/board_4layer.xml) and [golden stack](../import_cli/golden/board_4layer/stack.json) | Contract ruling and compatible public input/golden with all four fractions positive. |
| Core `mat.fr4` violates the homogenized `_eq` suffix | M1/M2/M3: [stack validator](../../../schema/src/stack.cpp) and [golden stack](../import_cli/golden/board_4layer/stack.json) | Agree on a material identity accepted by import, preview and solve. |
| Illustrative `mat.cu`/`mat.fr4` IDs contain forbidden dots | M3/material: [material schema](../../../material/schema/material.v1.schema.json) | Align the specified material IDs with the material-library contract. |
| Preview uses imported minimum 1; solve defaults to minimum 2 | M2/M3: [case loader](../../../apps/chipcae-solve/src/case_loader.cpp) and [golden stack](../import_cli/golden/board_4layer/stack.json) | Identical specified mesh settings must yield identical positive cell counts. |
| Independent readback exceeds relative 1e-12 | M3/M5: [Q2 manifest warpage](../../../apps/chipcae-solve/src/pipeline.cpp) and [vertex readback](../../../tools/vv/result_summary.py) | Align independent sampling/fit contracts while retaining the 1e-12 gate. |

The readback field was checked against `result_summary.py`: `warpage.w_bfp_m`
is independently computed; `warpage.manifest_warpage_pv_m` is copied. The script
keeps the former, with a regression that rejects a 6.8e-12 discrepancy.

After the owning modules resolve these contracts, run the exact diagnostic
through comparisons (a)–(g), demonstrate normal / heat-flux ×1.01 failure /
restored passing runs, and record fresh measurements. Only then restore
`chipcae_auto_tests(PREFIX f1130_ LABELS "integration;numerical;vv" TIMEOUT 1200)`
and its original gated executable environment. A passing records CTest alone
does not authorize registering or claiming full acceptance.

## Coverage limits

This card excludes ODB++, cut-cell solving, desktop I3 and CalculiX reference
solutions. Stage times are measured without a performance gate. The existing
nightly workflow owns numerical execution; no CI files change.
