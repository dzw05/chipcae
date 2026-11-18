# F-1130 uncut CLI acceptance

Current acceptance status: blocked. The required fixture fails the positive
copper-fraction check, and preview rejects its original Core material ID.
Serial/MPI numerical acceptance and the reference-mutation proof remain
incomplete; the required assertions remain in place.

The input is the existing public synthetic
`tests/integration/ecad/data/board_4layer.xml`, a 5 mm square board with
five physical layers: F.Cu, In1.Cu, Core, In2.Cu and B.Cu. Import uses the
existing `tests/integration/import_cli/material_map.json` verbatim:
`{"FR4":"mat.fr4","copper":"mat.cu"}`. These are the fixture and map
used by the import CLI's `board_4layer` golden-generation case.

The actual import arguments, relative to the repository and temporary
working directory respectively, are:

```text
chipcae-import <repo>/tests/integration/ecad/data/board_4layer.xml
  -o stack.json --polyset stack.polyset --report defects.json
  --material-map <repo>/tests/integration/import_cli/material_map.json
```

The test compares layer order, names, thicknesses (relative tolerance 1e-12)
and literal material IDs against
`tests/integration/import_cli/golden/board_4layer/stack.json`. Neither the
golden nor imported IDs are rewritten. Illustrative materials and the
analytic reference also use `mat.cu` and `mat.fr4`.

```text
existing board_4layer.xml → chipcae-import → stack.json + stack.polyset + defects.json
stack.json → tools/stack/copper_fraction.py → solve.stack.json + cf.json
solve.stack.json → chipcae-preview → preview.vtkhdf + JSON cell_count
shared case/stack/materials → chipcae-solve directly → ranks1/
same inputs, profile.ranks=2 → mpiexec/mpirun -n 2 → ranks2/
each completed solve → tools/vv/result_summary.py → summary.json
analytic T(z), mesh/QoI/readback/MPI comparisons → result.json → check_file()
```

Each invocation uses a fresh temporary directory and prints commands, stage
wall times and result records before cleanup. Material properties are
illustrative, copied from `test_ipc_uncut_chain.py`. Result records use
`data_class: synthetic` and `label: 实测`.

Each of the four copper layers must have a finite fraction in `(0,1]` and
retain its imported material ID. The converted `copper.metal` must be
`mat.cu`; its fraction must match the report (relative tolerance 1e-12).
Exact fraction references use the saved golden's F.Cu polygon area
0.307835 mm² / 25 mm² = 0.0123134 and zero for the three empty layers;
the mandatory positive-fraction check still rejects those empty layers.
Conductivities are 390 W/(m K) for copper and 0.3 W/(m K) for FR4, with
series mixing `k_eff = 1/(f/k_metal + (1-f)/k_matrix)`. Boundary conditions
are 298.15 K at zmin and convection to 348.15 K at zmax, h=1000 W/(m² K).
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
inspection starts.

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
ctest --test-dir "$build" -V -R '^f1130_'
ctest --test-dir "$build" --output-on-failure -j2
```

CTest registers exactly `f1130_uncut_cli_e2e`, with labels
`integration;numerical;vv`, timeout 1200 seconds and skip code 77. The same
script runs seven regression cases before the CLI chain: the six existing
bookkeeping cases plus literal material-ID rejection. No additional fixture
or test file is introduced. The bookkeeping checks cover missing acceptance
checks, empty numerical records, required MPI comparison, independent
per-run verdicts, temperature/readback/QoI inspection failures, and raising
after result writing and gating.

Executable variables are injected only when ECAD, the three CLI targets,
MFEM, MPI and HDF5 are enabled. JSONCONS must also be enabled for functional
preview/solve backends. Missing executable variables or numpy/h5py return
77. Configured commands that cannot run, time out or exit nonzero fail with
captured diagnostics. The first available `mpiexec`, then `mpirun`, is used
without retry. If neither exists, the MPI run/comparison is explicitly
omitted with `SKIP ranks=2: launcher not found`; full serial acceptance
remains mandatory.

## Verification (2026-11-18)

A Release build with ECAD, JSONCONS, MFEM, MPI and HDF5 enabled and the
optional core Kokkos backend disabled succeeded. Discovery found exactly
one F-1130 CTest target. All seven in-script regression cases passed.
A local check invoking the real importer rejected the previous dedicated
fixture/map and passed after restoring the required fixture/map and
comparison with the untouched golden.

The targeted CTest command above returned 8 (script exit 1). The imported
layers matched the golden, including literal material IDs. 实测 copper
fractions were F.Cu=0.012313399999999997, In1.Cu=0, In2.Cu=0 and B.Cu=0.
The existing XML supplies geometry only for F.Cu. The unchanged positive
fraction assertion therefore fails at In1.Cu. The failing result record
passes the existing data-class gate; it records import completion and the
incomplete downstream checks, with no fabricated QoIs or temperature error.
实测 stage times were import 0.450491 s, conversion 0.079964 s and total
0.531376 s.

A separate diagnostic using the same unmodified import/conversion outputs
also confirmed that preview exits 1: Core has `kind: Homogenized` and
`material: mat.fr4`, while the stack parser requires an `_eq` suffix.
The acceptance test stops at the fraction assertion before preview.
Serial/MPI solves, readback and the reference-flux ×1.01 negative experiment
are blocked for the mandated fixture. Earlier successful numerical and
mutation measurements used the removed custom fixture and remapped IDs;
they do not validate this required chain. No tolerance or required positive
fraction check was relaxed. Product code, existing fixtures and golden
files remain unchanged, as required by the brief.

The full Release suite (`ctest --test-dir "$build" --output-on-failure -j2`)
returned 8: 231 total, 221 passed, 8 skipped and 2 failed, in 1230.46 s.
The failures were `f1130_uncut_cli_e2e` (the zero copper fractions above)
and the unchanged `chain_ipc_uncut_chain` (line 53 expects `mat.fr4` on
copper layers, while conversion preserves `mat.cu`). The full suite is
not green. In this run `solve_ghost_scan` passed in 268.92 s and
`k1_preview_mpi_surface` passed in 46.37 s; their earlier failures do not
describe this verification. Tests and product sources outside the brief's
allowlist were not changed.

## Coverage limits

This card excludes ODB++, cut-cell solving, desktop I3 and CalculiX reference
solutions. Stage times are measured and recorded without a performance gate.
The existing nightly workflow owns numerical execution; no CI files change.
