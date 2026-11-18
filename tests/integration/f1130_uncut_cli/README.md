# F-1130 uncut CLI acceptance

CTest registers `f1130_uncut_cli_e2e` through `chipcae_auto_tests`, with labels
`integration;numerical;vv`, timeout 1200 seconds and skip return code 77.
The fixture gate has been removed: configured numerical builds execute the
chain by default. Missing executable environment or numpy/h5py still returns
77. The separately runnable `f1130_uncut_cli_records` retains the assertion
and verdict regressions; passing them alone does not establish E2E acceptance.

The supplied review's V&V rulings and the later 03:55 material-ID supplement
are implemented in the inputs and comparisons below. The supplement supersedes
the earlier dotted IDs; the material schema remains unchanged.
**Numerical acceptance remains blocked:** both solves now complete, but their
temperatures disagree with the required FR4-matrix reference. See Verification
for the normal, perturbed and restored measurements.

## Inputs and chain

Input: `tests/integration/ecad/data/board_4layer.xml`, the unchanged public
synthetic 5 mm square board. Import uses this directory's [material_map.json](material_map.json):
`{"FR4":"fr4_eq","copper":"cu"}`.

```text
chipcae-import <repo>/tests/integration/ecad/data/board_4layer.xml
  -o stack.json --polyset stack.polyset --report defects.json
  --material-map <repo>/tests/integration/f1130_uncut_cli/material_map.json
```

The golden-generation map was `{"FR4":"mat.fr4","copper":"mat.cu"}`.
Only a deep copy of the golden is normalized with
`{"mat.fr4":"fr4_eq","mat.cu":"cu"}` before comparing layer names, order, materials and
thicknesses. The original XML, import CLI map and golden remain unchanged.

```text
board_4layer.xml + F1130 map → chipcae-import → stack.json + sidecar + defects
stack.json → compare against a normalized copy of the original import golden
stack.json → tools/stack/copper_fraction.py → solve.stack.json + cf.json
solve.stack.json → chipcae-preview → preview.vtkhdf + JSON cell_count
shared case/stack/materials → chipcae-solve directly → ranks1/
same input bytes, profile.ranks=2 → mpiexec/mpirun -n 2 → ranks2/
each completed solve → tools/vv/result_summary.py → summary.json
analytic T(z), mesh/QoI/readback/MPI checks → result.json → check_file()
```

Each invocation uses fresh temporary directories. Shared solver input bytes
must remain unchanged; profiles differ only in ranks. Commands, measured stage
wall times and result records are printed before cleanup. Records use schema
`chipcae.vv_result/0.2`, `data_class: synthetic` and `label: 实测`. Incomplete
checks force a failing verdict, including failures before solving. The field
reference uses `reference.value: null`; `date` is runtime YYYYMMDD metadata.

## Comparisons

- Thickness relative tolerance remains 1e-12. The material substitution applies
  only to the golden copy; unexpected material identities still fail.
- F.Cu must be finite, in `(0,1]`, and agree with the independent area reference
  0.307835 mm² / 25 mm² = 0.0123134 (relative 1e-12).
  In1.Cu, In2.Cu and B.Cu must equal zero, as required by the ruling for the
  fixture's absent artwork. Converted fractions, order and identities are checked.
- Preview uses `--nx 4 --ny 4 --levels 0 --max-ratio 100`. The case mesh uses
  `{"type":"aligned_cartesian_cut","nx":4,"ny":4,"levels":0,"max_ratio":100,"min_elems_per_layer":1}`.
  This explicitly matches preview's per-layer minimum without changing the
  solve default. Cut count must be zero, all hex cell_kind values zero, and the
  positive hex count must directly equal preview's cell_count.
- Hex point temperatures must match the independent 1D series-resistance
  reference within 1e-8 K. Illustrative properties retain copper k=390 W/(m K)
  and FR4 k=0.3 W/(m K), with material identities `cu` and `fr4_eq`.
  Boundary conditions remain zmin=298.15 K and zmax convection h=1000 W/(m² K),
  T_inf=348.15 K, with 3-2-1 mechanical constraints.
  The analytic copper-layer conductivity uses
  `k_eff=1/(f/k_metal+(1-f)/conductivity['fr4_eq'])`, including f=0.
  Then `q=50/(sum(thickness/k_eff)+1/1000)` and
  `T(z)=298.15+q*sum(clipped_layer_depth(z)/k_eff)`.
- warpage_pv and U_mag_max must be finite and positive; T_max must be finite.
  Readback `warpage.manifest_warpage_pv_m` must match manifest warpage_pv within
  relative 1e-12. Independent `warpage.w_bfp_m` is printed for diagnostics.
  MPI QoIs must agree within relative 1e-8.
- Every result record must pass `tools.vv.check_result_data_class.check_file()`.

The numerical regression uses hand-derived temperatures at the bottom,
midpoint and top of a 1 mm FR4 layer and copper layers with f=0 and f=0.5.
It rejects temperature errors, copied-manifest mismatches and invalid QoIs,
and accepts an independent w_bfp value that differs from manifest warpage.
Other regressions cover input settings, golden preservation, invalid fractions,
retired-gate behavior and incomplete/failing verdicts.

## Running

Use the shared build-slot wrappers on PATH, with at most two jobs. Supply
local dependency locations through CMake options as needed:

```sh
cmake -S . -B "$build" -DCMAKE_BUILD_TYPE=Release \
  -DCHIPCAE_BUILD_TESTS=ON -DCHIPCAE_ENABLE_ECAD=ON \
  -DCHIPCAE_ENABLE_JSONCONS=ON -DCHIPCAE_ENABLE_MFEM=ON \
  -DCHIPCAE_ENABLE_MPI=ON -DCHIPCAE_ENABLE_HDF5=ON
cmake --build "$build" -j2
ctest --test-dir "$build" -V -R '^f1130_'
ctest --test-dir "$build" --output-on-failure -j2
```

CMake injects `CHIPCAE_IMPORT_EXE`, `CHIPCAE_PREVIEW_EXE` and
`CHIPCAE_SOLVE_EXE` only when ECAD, MFEM, MPI and HDF5 are enabled and all three
executable targets exist. Configured command failures print the command, exit
code and stdout/stderr and fail acceptance; each command has a 600 second
timeout. The first available `mpiexec`, then `mpirun`, is used without retry.
The existing mandatory-launcher and parallel-completion checks remain active.
Python 3.10 or newer is required by the strict zip comparisons.

## Verification (2026-11-19)

Release configure and build both exit 0, with tests, ECAD, JSONCONS, MFEM,
MPI and HDF5 enabled and optional Kokkos disabled:

```sh
cmake -S . -B "$build" -DCMAKE_BUILD_TYPE=Release
cmake --build "$build" -j2
```

The updated regression expectations first fail against the old implementation:
the library still emits dotted IDs, the golden copy retains `mat.cu`, and the
conductivity lookup cannot inspect `cu`/`fr4_eq` layers. After the fix, all 12
regressions pass. The local map, generated library, analytical conductivity
keys and regression inputs now use the schema-valid IDs `cu`/`fr4_eq`.
Only the golden copy is normalized; the golden file is unchanged on disk.

The normal command is `ctest --test-dir "$build" -V -R '^f1130_'`.
It exits 8: the records test passes, and the E2E fails at temperature check (c)
after **both serial and MPI solves/readbacks complete successfully**. There is
no material-ID-pattern error. 实测 import 0.472369 s, fraction conversion
0.127180 s, preview 1.627135 s, serial solve/readback 2.245632/0.356207 s,
MPI solve/readback 2.542167/0.237694 s, total chain 7.619526 s.

All three real-chain runs below use the unchanged public synthetic fixture.
Import matches the normalized golden. 实测 F.Cu fraction is
0.012313399999999997; In1.Cu, In2.Cu and B.Cu fractions are zero.
Both runs have 80 hex cells, matching preview's 80 cells, zero cut cells and
zero hex cell_kind values. 实测 serial and MPI QoIs are identical:
`warpage_pv=8.118595078914043e-06 m`, `U_mag_max=8.150512393406987e-06 m`,
`T_max=318.1548915099446 K`. Readback relative error and all three parallel
relative errors are zero. Independent `w_bfp_m=8.118595078858583e-06 m` is
printed without gating. Both failing result records pass `check_file()`.

For the local negative experiment, only the real chain's reference heat flux
was temporarily multiplied by 1.01; the independent regression fixtures were
unchanged. Negative and restored runs both use:

```sh
ctest --test-dir "$build" -V -R '^f1130_uncut_cli_e2e$'
```

| Run | CTest exit | Solve/readback ranks 1 and 2 | 实测 maximum temperature error (both ranks) | 实测 total chain |
| --- | --- | --- | --- | --- |
| Normal | 8 | Completed, all commands exit 0 | 5.22503105806652 K | 7.619526 s |
| Reference heat flux ×1.01 | 8 | Completed, all commands exit 0 | 5.4773302837454025 K | 6.314309 s |
| Original source restored byte for byte | 8 | Completed, all commands exit 0 | 5.22503105806652 K | 6.049460 s |

The negative run now demonstrates a post-solve failure at (c). However, the
restored run also fails the unchanged 1e-8 K limit: the required restored
passing run and full numerical acceptance remain **blocked**.

The remaining mismatch is the Patterned-layer matrix contract explicitly
deferred by the architect's 03:55 supplement. The importer and fraction
conversion retain `material: cu` and `copper.metal: cu` for copper layers.
The [solver mixture](../../../apps/chipcae-solve/src/pipeline.cpp) selects
`materials.at(id)` for the layer's material as its matrix. Its through-thickness
formula uses copper conductivity for both constituents, including at f=0.
The required independent reference instead uses `fr4_eq` as its matrix.
Changing that reference, rewriting converted material identities, altering
properties or increasing the tolerance would hide the unresolved contract.
This needs the separate signed I1/M1/M3 matrix-semantics change; product,
schema, conversion-tool, original-golden and legacy-test files are outside
this task's permitted scope.

The full numerical Release command
`ctest --test-dir "$build" --output-on-failure -j2` exits 8 after 1177.45 s:
**232 total, 222 passed, 8 skipped, 2 failed**. The failures are:

- `f1130_uncut_cli_e2e`: both completed runs fail (c) with the matrix mismatch
  documented above. All 12 assertion/verdict regressions pass, including the
  separately registered records test.
- Unchanged `chain_ipc_uncut_chain`: line 53 expects `mat.fr4` while imported
  F.Cu retains `mat.cu`. The supplement assigns this migration to the separate
  `fix-import-eq-maps` follow-up; that test is outside this task's scope.

The suite uses a fresh private `TMPDIR` and one thread per numerical library.
`event_log` passes in 0.72 s; `solve_ghost_scan` passes in 258.00 s with its
original 300 second timeout. Full-suite acceptance is not achieved.

`git diff --check` is clean. Pinned clang-format is 23.1.5; this revision
changes no C/C++ files.

## Coverage limits

This card excludes ODB++, cut-cell solving, desktop I3 and CalculiX reference
solutions. Stage times are measured without a performance gate. No product
code, original golden/fixture, numerical threshold, timeout or CI file changes.
