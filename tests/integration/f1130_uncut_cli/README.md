# F-1130 uncut CLI acceptance

CTest registers `f1130_uncut_cli_e2e` through `chipcae_auto_tests`, with labels
`integration;numerical;vv`, timeout 1200 seconds and skip return code 77.
The fixture gate has been removed: configured numerical builds execute the
chain by default. Missing executable environment or numpy/h5py still returns
77. The separately runnable `f1130_uncut_cli_records` retains the assertion
and verdict regressions; passing them alone does not establish E2E acceptance.

The supplied review's V&V rulings are implemented in the inputs and
comparisons below.
**Full acceptance remains blocked by the material-library ID contract:** both
solver runs reject the required `mat.cu` ID before solving. This is a failing
acceptance result, and the required negative/restored experiment is incomplete.

## Inputs and chain

Input: `tests/integration/ecad/data/board_4layer.xml`, the unchanged public
synthetic 5 mm square board. Import uses this directory's [material_map.json](material_map.json):
`{"FR4":"mat.fr4_eq","copper":"mat.cu"}`.

```text
chipcae-import <repo>/tests/integration/ecad/data/board_4layer.xml
  -o stack.json --polyset stack.polyset --report defects.json
  --material-map <repo>/tests/integration/f1130_uncut_cli/material_map.json
```

The golden-generation map was `{"FR4":"mat.fr4","copper":"mat.cu"}`.
Only a deep copy of the golden is normalized with
`{"mat.fr4":"mat.fr4_eq"}` before comparing layer names, order, materials and
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
  and FR4 k=0.3 W/(m K), with material identities `mat.cu` and `mat.fr4_eq`.
  Boundary conditions remain zmin=298.15 K and zmax convection h=1000 W/(m² K),
  T_inf=348.15 K, with 3-2-1 mechanical constraints.
  The analytic copper-layer conductivity uses
  `k_eff=1/(f/k_metal+(1-f)/conductivity['mat.fr4_eq'])`, including f=0.
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

Release configure and build succeed with ECAD, JSONCONS, MFEM, MPI and HDF5
available and optional Kokkos disabled. The revised regressions first failed
against the old comparisons, IDs, mesh and gate. After correction, all 12
regressions pass. CTest registration retains the executable injection, labels,
timeout and dependency-only skip path, with no fixture gate.
The optional-backend Release configure/build also succeed; its records test
passes and its E2E returns 77 because executable injection is unavailable.

The initial normal command was
`ctest --test-dir "$build" -V -R '^f1130_'`: exit 8 (records pass; E2E fails).
实测 import 0.414621 s, fraction conversion 0.055050 s, preview 1.512144 s,
serial solve command 1.575522 s, MPI solve command 2.074719 s, total chain
5.634468 s. Import matches the normalized golden. 实测 F.Cu fraction is
0.012313399999999997; the other three copper fractions are zero. Preview
succeeds and reports 80 cells. Both solve commands exit 14:

```text
chipcae-solve: material mat.cu /id: String 'mat.cu' does not match pattern '^[a-z0-9_-]+$'.
```

The [material schema](../../../material/schema/material.v1.schema.json) forbids
dots in IDs; the supplied review requires `mat.cu` and `mat.fr4_eq`. Changing
the schema or solver is outside the brief's permitted files. No solver output, temperature
error, readback or parallel agreement is claimed. Both emitted fail records
pass the data-class gate.

The architect's later 03:55 supplement to the PR #139 ruling instead prescribes
`cu` and `fr4_eq`, keeping the schema unchanged. That conflicts with the supplied
review's explicit IDs; this revision retains the requested IDs pending clarification.
The supplement also leaves the matrix contract below for a separate signed change.

Static follow-up: the [solver mixture](../../../apps/chipcae-solve/src/pipeline.cpp)
uses each layer's material as its matrix. Imported copper layers retain
`mat.cu`, while the ruled analytic reference requires an FR4 matrix even at
f=0. The solve-stack matrix identity and solver contract also need agreement
before numerical acceptance; the current run stops before exercising this path.

A local, uncommitted experiment temporarily multiplied the real chain's
reference heat flux by 1.01, leaving the separate harness checks unchanged.
Negative and restored commands were both
`ctest --test-dir "$build" -V -R '^f1130_uncut_cli_e2e$'`.
Both exit 8 on the same solver input error, before temperature inspection.
The source was restored byte for byte. These attempts do **not** satisfy the
required post-solve temperature failure and restored passing run; that merge
condition remains open.

The full numerical Release command
`ctest --test-dir "$build" --output-on-failure -j2` exits 8 after 1148.95 s:
**232 total, 222 passed, 8 skipped, 2 failed**. The failures are
`f1130_uncut_cli_e2e` (the ID-pattern error above) and the unchanged
`chain_ipc_uncut_chain` (line 53 expects `mat.fr4` but converted F.Cu retains
`mat.cu`). The latter is outside this brief's permitted files and is assigned
to the separate `fix-import-eq-maps` follow-up. `event_log` passes with a fresh
private `TMPDIR`; `solve_ghost_scan` passes in 237.41 s with its original
300 second timeout. Full-suite acceptance is not achieved.

`git diff --check` is clean. Pinned clang-format is 23.1.5; this revision
changes no C/C++ files.

## Coverage limits

This card excludes ODB++, cut-cell solving, desktop I3 and CalculiX reference
solutions. Stage times are measured without a performance gate. No product
code, original golden/fixture, numerical threshold, timeout or CI file changes.
