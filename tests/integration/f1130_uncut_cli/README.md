# F-1130 uncut CLI acceptance

This test uses the dedicated public synthetic `board_4layer.xml` beside the
script. The five physical layers are F.Cu, In1.Cu, Core, In2.Cu and B.Cu.
The centered 1 mm square board has copper rectangles of 0.12, 0.24, 0.36
and 0.48 mm², giving independently checked fractions 0.12, 0.24, 0.36 and
0.48. Every copper layer
must still have a finite fraction in `(0,1]`. The footprint keeps cell aspect
ratios modest (below 14) for the strict plane-fit readback comparison, and
centering avoids unnecessary coordinate offsets in the plane fits.

The original ECAD fixture has three empty copper layers and cannot satisfy
that requirement. It and the import CLI golden remain unchanged. The new
fixture retains the golden's layer order and thicknesses. Import uses the
explicit material map `{"FR4":"mat_fr4_eq","copper":"mat_cu"}` so Core
satisfies preview's existing Homogenized material naming contract, and both
IDs satisfy the material schema's alphanumeric/underscore restriction. Import
metadata is compared to the original golden with these declared ID
mappings applied in memory. Converted layers must retain the imported IDs;
no post-import material or kind rewriting is performed.

```text
board_4layer.xml → chipcae-import → stack.json + stack.polyset + defects.json
stack.json → tools/stack/copper_fraction.py → solve.stack.json + cf.json
solve.stack.json → chipcae-preview → preview.vtkhdf + JSON cell_count
shared case/stack/materials → chipcae-solve directly → ranks1/
same inputs, profile.ranks=2 → mpiexec/mpirun -n 2 → ranks2/
each completed solve → tools/vv/result_summary.py → summary.json
analytic T(z), mesh/QoI/readback/MPI comparisons → result.json → check_file()
```

Each invocation uses a fresh temporary directory and prints commands, stage
wall times and result records before cleanup. Material properties are
illustrative, using the same values as `test_ipc_uncut_chain.py`. Result
records use `data_class: synthetic` and `label: 实测`.

Copper layers retain `material: mat_cu`, as emitted by the converter.
Conductivities are 390 W/(m K) for copper and 0.3 W/(m K) for FR4, with
series mixing `k_eff = 1/(f/k_metal + (1-f)/k_matrix)`. Boundary conditions
are 298.15 K at zmin and convection to 348.15 K at zmax, h=1000 W/(m² K).
The independent reference is `q=50/(sum(thickness/k_eff)+1/1000)` and
`T(z)=298.15+q*sum(clipped_layer_depth(z)/k_eff)`.

Preview and solve use nx=4, ny=4, levels=0, max_ratio=100. The case explicitly
sets `min_elems_per_layer=1` to match preview's imported layer minima, rather
than solve's default of two. Acceptance requires zero cut cells, all hex
cell_kind=0, positive hex count equal to preview's
cell_count, matching z-extents, finite temperatures with maximum absolute
reference error ≤1e-8 K, positive finite warpage_pv and U_mag_max, finite
T_max, readback warpage relative error ≤1e-12, and ranks=2/1 QoI relative
errors ≤1e-8 using the absolute ranks=1 value as denominator. Profiles differ
only in ranks; shared input bytes are checked after each solve/readback.
Both available solves/readbacks finish before numerical inspection starts.

Each result's `run.acceptance_checks` records its completed import, fraction,
preview, solve/readback, unchanged-input and numerical checks. Numerical
completion is recorded only after every inspection assertion passes. When
MPI is available, both records also require the parallel comparison. A
missing check makes that record fail, including when no assertion message
was accumulated. Each run has its own failures; a serial failure does not
assign the MPI verdict. Shared setup and parallel-comparison failures apply
to both runs. All records are written and gated before accumulated failures
are raised, ensuring a nonzero process exit.

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

CTest registers `f1130_uncut_cli_e2e` and the bookkeeping regression test
`f1130_acceptance_records`, with labels `integration;numerical;vv`, timeout
1200 seconds and skip code 77. Executable variables are injected for the E2E
test only when ECAD, the three CLI targets, MFEM, MPI and HDF5 are enabled.
JSONCONS must also be enabled for functional preview/solve backends.
Missing executable variables or numpy/h5py return 77. Configured commands
that cannot run, time out or exit nonzero fail with captured diagnostics.
The first available `mpiexec`, then `mpirun`, is used without retry. If neither
exists, the MPI run/comparison is explicitly omitted with
`SKIP ranks=2: launcher not found`; full serial acceptance remains mandatory.

The bookkeeping regressions exercise missing acceptance checks, empty
numerical records, required MPI comparison, independent per-run verdicts,
inspection failures for temperature/readback/QoI, and raising accumulated
failures after result writing and gating.

## Verification (2026-11-18)

A complete Release build with ECAD, JSONCONS, MFEM, MPI and HDF5 enabled,
and the optional Kokkos execution backend disabled, succeeded. Both F-1130
CTest targets passed, including all six bookkeeping regression cases.
The reference-mutation experiment used the real CLI chain at both ranks:

| Reference | CTest exit | Serial / MPI verdict | Maximum temperature error |
| --- | --- | --- | --- |
| Normal | 0 | pass / pass | 1.61436e-10 K |
| Heat flux multiplied by 1.01 locally | 8 | fail / fail | 0.200049 K |
| Source restored byte for byte | 0 | pass / pass | 1.61436e-10 K |

Each run had 80 hex cells, zero cut cells and a matching preview count.
Normal/restored readback relative error was 5.19369e-14; every serial/MPI QoI
relative difference was zero. All acceptance flags were completed in both
passing records. The mutation completed both solves/readbacks and wrote and
gated both failing records before raising the temperature assertions. The
mutation is absent from the committed source. No tolerance was relaxed.

The full suite returned CTest exit 8: 232 total, 220 passed, 8 skipped and
4 failed. Its F-1130 cell-count setup failure was subsequently fixed and the
final normal/mutation/restored evidence above uses the completed fixture.
The initial `k1_preview_mpi_surface` JSON abort passed on an isolated rerun.
Two unchanged tests remain failing: `solve_ghost_scan` times out at 300 s
(including a rerun with BLAS limited to one thread), and
`chain_ipc_uncut_chain` expects `mat.fr4` where the converter preserves
`mat.cu`. The full suite is not green; these tests and product code were not
changed as part of the three confirmed review fixes.

## Coverage limits

This card excludes ODB++, cut-cell solving, desktop I3 and CalculiX reference
solutions. Stage times are measured and recorded without a performance gate.
The existing nightly workflow owns numerical execution; no CI files change.
