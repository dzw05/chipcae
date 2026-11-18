# F-1130 uncut CLI acceptance

The test now reaches preview, both real solves and both readbacks with a
populated public synthetic fixture derived from
`tests/integration/ecad/data/board_4layer.xml`. Positive copper fractions and
schema-compatible material IDs are resolved. Numerical acceptance remains
blocked by the existing Q2-manifest/exported-vertex warpage discrepancy at the
required 1e-12 relative readback threshold. The original 5 mm domain and all
acceptance thresholds are retained.

## Inputs and fixture compatibility

The original input is a public synthetic 5 mm square board with five physical
layers: F.Cu, In1.Cu, Core, In2.Cu and B.Cu. The first import uses the existing
`tests/integration/import_cli/material_map.json` verbatim:
`{"FR4":"mat.fr4","copper":"mat.cu"}`. These are the original import CLI
`board_4layer` golden-generation inputs. Layer order, names, thicknesses
(relative tolerance 1e-12) and literal material IDs must match
`tests/integration/import_cli/golden/board_4layer/stack.json`.

That source has artwork only on F.Cu, and its Homogenized Core's `mat.fr4` ID
fails preview's `_eq` rule. For numerical acceptance the script writes
`populated_board_4layer.xml` in its temporary directory. It retains the source's
5 mm square outline, original F.Cu trace, layer definitions, physical order and
all five thicknesses. It adds three rectangles to the previously empty layers:

| Layer | Geometry (mm) | Area (mm²) / board area (25 mm²) |
| --- | --- | --- |
| F.Cu | Original golden trace | 0.307835 / 25 = 0.0123134 |
| In1.Cu | x=1–4, y=0.5–1.5 | 3 / 25 = 0.12 |
| In2.Cu | x=1–4, y=0.5–2.5 | 6 / 25 = 0.24 |
| B.Cu | x=1–4, y=0.5–3.5 | 9 / 25 = 0.36 |

Its explicit temporary map is
`{"FR4":"mat_fr4_eq","copper":"mat_cu"}`. These IDs satisfy the material
library's spelling rule and the stack's Homogenized suffix rule. The variant's
import is checked against a separate expected stack with these mapped IDs,
the original layer order/thicknesses, and the unchanged outline. The existing
XML, map, golden, converter and product code are unchanged. The original
import-to-golden comparison still requires the original literal IDs.

A local diagnostic using a 1 mm square numerical variant passed all thresholds,
including readback (1.03e-13 relative), but it changes the mandated domain.
That variant is not delivered. A separate 5 mm FR4-matrix diagnostic still
failed readback (3.69e-12 relative). Resolving the remaining blocker requires a
fixture/contract ruling or product changes outside this brief's file scope;
the required assertions remain active.

The actual import commands, relative to the repository and temporary working
directory respectively, are:

```text
chipcae-import <repo>/tests/integration/ecad/data/board_4layer.xml
  -o stack.json --polyset stack.polyset --report defects.json
  --material-map <repo>/tests/integration/import_cli/material_map.json
chipcae-import populated_board_4layer.xml
  -o stack.json --polyset stack.polyset --report defects.json
  --material-map solver_material_map.json
```

```text
original board_4layer.xml + original map → chipcae-import → untouched golden comparison
source board + three supplemental rectangles → temporary populated_board_4layer.xml
populated board + solver map → chipcae-import → stack.json + stack.polyset + defects.json
stack.json → tools/stack/copper_fraction.py → solve.stack.json + cf.json
solve.stack.json → chipcae-preview → preview.vtkhdf + JSON cell_count
shared case/stack/materials → chipcae-solve directly → ranks1/
same inputs, profile.ranks=2 → mpiexec/mpirun -n 2 → ranks2/
each completed solve → tools/vv/result_summary.py → summary.json
analytic T(z), mesh/QoI/readback/MPI comparisons → result.json → check_file()
```

## Acceptance checks

Each invocation uses a fresh temporary directory and prints commands, stage
wall times and result records before cleanup. Material properties are
illustrative, copied from `test_ipc_uncut_chain.py`, with explicit mapped IDs.
Result records use `data_class: synthetic` and `label: 实测`.

All four copper fractions must be finite and in `(0,1]`, match the independent
trace/rectangle-area references above (relative tolerance 1e-12), and appear in the
converted stack's `copper.fraction`. Conversion must preserve imported layer
order and material IDs, retain `copper.metal: mat_cu`, and remove polygon refs.
Conductivities are 390 W/(m K) for copper and 0.3 W/(m K) for FR4, with
series mixing `k_eff = 1/(f/k_metal + (1-f)/k_matrix)`. The importer assigns
`mat_cu` to both the base and metal of each copper layer; the reference uses
those actual material assignments. Boundary conditions are 298.15 K at zmin
and convection to 348.15 K at zmax, h=1000 W/(m² K).
The independent reference is `q=50/(sum(thickness/k_eff)+1/1000)` and
`T(z)=298.15+q*sum(clipped_layer_depth(z)/k_eff)`.

Preview and solve use nx=4, ny=4, levels=0, max_ratio=100. The case explicitly
sets `min_elems_per_layer=1` to match preview's imported layer minima.
Acceptance requires zero cut cells, all hex cell_kind=0, positive hex count
equal to preview's cell_count, matching z-extents, finite temperatures with
maximum absolute reference error ≤1e-8 K, positive finite warpage_pv and
U_mag_max, finite T_max, readback warpage relative error ≤1e-12, and ranks=2/1
QoI relative errors ≤1e-8 using the absolute ranks=1 value as denominator.
Profiles differ only in ranks; shared input bytes are checked after each
solve/readback. Both available solves/readbacks finish before numerical
inspection starts. All thresholds and required checks are unchanged.

Each result's `run.acceptance_checks` records its completed import, fraction,
preview, solve/readback, unchanged-input and numerical checks. Numerical
completion is recorded only after every inspection assertion passes. When
MPI is available, both records also require the parallel comparison. A
missing check makes that record fail, including when no assertion message
was accumulated. Each run has its own failures. Shared setup and
parallel-comparison failures apply to both runs. All records are written
and gated before accumulated failures are raised, ensuring a nonzero exit.

## Running

Use plain `cmake` and `ctest` through the environment's build-slot wrappers.
Keep the wrapper directory first in PATH and allow queued commands to wait.
Configure Release, tests, ECAD, JSONCONS, MFEM, MPI and HDF5; provide dependency
locations through the appropriate CMake options. Kokkos is optional.

```sh
cmake -S . -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCHIPCAE_BUILD_TESTS=ON -DCHIPCAE_ENABLE_ECAD=ON \
  -DCHIPCAE_ENABLE_JSONCONS=ON -DCHIPCAE_ENABLE_MFEM=ON \
  -DCHIPCAE_ENABLE_MPI=ON -DCHIPCAE_ENABLE_HDF5=ON
cmake --build "$build" -j2
ctest --test-dir "$build" -N -V -R '^f1130_'
ctest --test-dir "$build" -V -R '^f1130_uncut_cli_e2e$'
ctest --test-dir "$build" --output-on-failure -j2
```

CTest registers exactly `f1130_uncut_cli_e2e`, with labels
`integration;numerical;vv`, timeout 1200 seconds and skip code 77. The same
script runs seven regression cases before the CLI chain. They cover literal
material-ID rejection, missing acceptance checks, empty numerical records,
required MPI comparison, independent per-run verdicts,
temperature/readback/QoI inspection failures, and raising after result writing
and gating. The module can also be imported for plain unittest collection;
its repository root is derived from its file location.

Executable variables are injected only when ECAD, the three CLI targets,
MFEM, MPI and HDF5 are enabled. JSONCONS must also be enabled for functional
preview/solve backends. Missing executable variables or numpy/h5py return
77. Configured commands that cannot run, time out or exit nonzero fail with
captured diagnostics. The first available `mpiexec`, then `mpirun`, is used
without retry. If neither exists, the MPI run/comparison is explicitly
omitted with `SKIP ranks=2: launcher not found`; full serial acceptance
remains mandatory.

## Verification (2026-11-18)

Release configure and build succeeded with ECAD, JSONCONS, MFEM, MPI and HDF5
enabled and optional core Kokkos disabled. The original failing CTest was
reproduced before fixture changes. All seven regression cases passed. Plain
unittest collection passes, and execution without configured CLI variables
returns the expected 77, including without a repository-root argument.

The normal 5 mm CTest command is:

```sh
ctest --test-dir "$build" -V -R '^f1130_uncut_cli_e2e$'
```

It exits 8 (script exit 1). 实测 fractions are F.Cu=0.012313399999999997,
In1.Cu=0.12, In2.Cu=0.24 and B.Cu=0.36. Preview exits 0 and reports 80 cells.
Both actual solves and readbacks exit 0 and print `COMPLETED`. Each solve has
80 hex cells, zero cut cells and maximum temperature error 1.76101e-10 K.
Both ranks produce warpage_pv=8.118595078913672e-6 m,
U_mag_max=8.150512393406525e-6 m and T_max=318.1548915099446 K. All three MPI
QoI relative differences are zero. Shared input bytes remain unchanged.

Readback returns 8.118595078858198e-6 m, a relative difference of
6.8329410744551444e-12, above the unchanged 1e-12 requirement. The solver
fits its manifest warpage using Q2 top nodes; readback fits the exported
vertices. Both result records correctly have verdict `fail`, numerical
completion remains false, and both records pass the data-class gate. The
chain now exercises actual solver output through all downstream checks;
a passing numerical verdict is not claimed.

The required local reference heat-flux ×1.01 experiment uses a temporary copy
of the script with its repository root set explicitly. Only its analytic
`heat_flux = 50 / ...` is changed to `heat_flux = 1.01 * 50 / ...`. A temporary
CTest invokes its `main()` directly through:

```sh
python3 -c "import runpy; module = runpy.run_path('negative.py'); raise SystemExit(module['main']())"
```

This bypasses the synthetic bookkeeping regressions, which also use the
inspection function, and retains the real importer/preview/solver/readback
commands and their environment. The repository source is unchanged throughout
the copied-script experiment. The commands and measured errors were:

| Experiment | Command | CTest / script exit | 实测 maximum temperature error, ranks=1/2 |
| --- | --- | --- | --- |
| Normal | `ctest --test-dir "$build" -V -R '^f1130_uncut_cli_e2e$'` | 8 / 1 | 1.76101e-10 K each |
| Copied reference heat flux ×1.01 | `ctest --test-dir "$negative_build" -V` (temporary CTest above) | 8 / 1 | 0.2000489153 K each |
| Unmodified source after negative | same command as Normal | 8 / 1 | 1.76101e-10 K each |

Normal and restored runs fail the readback requirement. The negative run
completes both real solves/readbacks before the temperature assertion fails;
its failing result records also pass the data-class gate. The restored CTest
took 7.68 s. A passing normal / negative / passing restored sequence remains
blocked for the mandated domain. The compact diagnostic's passing sequence
does not establish acceptance for the delivered 5 mm case.

The full Release suite, `ctest --test-dir "$build" --output-on-failure -j2`,
exited 8: **231 total, 219 passed, 8 skipped, 4 failed**, in 1243.86 s.
The failed tests were:

- `f1130_uncut_cli_e2e`: the readback discrepancy detailed above.
- `chain_ipc_uncut_chain`: the pre-existing material assertion detailed below.
- `event_log`: too many salvage files at the fixed shared temporary filename.
  A rerun with a fresh private `TMPDIR` passed in 0.76 s.
- `k1_preview_mpi_surface`: JSON parsing abort; CLI stdout contained a
  non-JSON diagnostic prefix. It also failed with a private `TMPDIR` in
  43.62 s. The original artifact had a solder-warning prefix; the isolated
  artifact had a `run_config levels=` prefix. Its test and preview/schema
  sources are unchanged from branch baseline b7bef7f.

The isolated rerun command was
`ctest --test-dir "$build" -V -j2 -R '^(event_log|k1_preview_mpi_surface)$'`,
with `TMPDIR` set to a fresh directory. It exited 8: one passed, one failed.
The suite remains red; no failures were skipped or thresholds relaxed.

The legacy `chain_ipc_uncut_chain` failure was independently reproduced before
this change: line 53 expects copper-layer base `mat.fr4`, while conversion
preserves imported `mat.cu`. Its test, converter and importer serialization
are identical to branch baseline b7bef7f; their last relevant change predates
this PR. The brief prohibits changing this existing test or product/tooling
code. A separately tested local fixture patch passes with the same thresholds,
but remains outside the repository pending explicit scope approval.

Review disposition: the positive-fraction and Core-ID blockers are resolved.
All downstream acceptance checks now execute against real serial/MPI output,
including the reference-mutation proof. The MUST requiring a green chain
remains blocked by readback, and the all-CTest-green requirement is unmet.
No substantive CodeBuddy MUST was rejected as incorrect. Its statement that
an allowlisted file cannot address fixture compatibility was too broad:
the allowed test script can generate the explicit populated fixture and map
without changing existing public inputs or product code.

## Coverage limits

This card excludes ODB++, cut-cell solving, desktop I3 and CalculiX reference
solutions. Stage times are measured and recorded without a performance gate.
The existing nightly workflow owns numerical execution; no CI files change.
