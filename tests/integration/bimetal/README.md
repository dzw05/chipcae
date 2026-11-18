# Synthetic free bilayer acceptance

The square is [0,1]² m with conforming, perfectly bonded explicit layers,
bottom to top: (100 GPa, 5e-6/K, 0.02 m) and
(200 GPa, 15e-6/K, 0.02 m), both nu=0. Temperature is 400 → 300 K.
The reference uses `tools.vv.derive_cards.timoshenko`, with
W_ref = |kappa| L²/4 for top-surface best-fit-plane peak-to-valley warpage.
No vendor data or external solver reference is used.

Each temporary workspace contains the following subdirectories. In each,
`case.json`, `stack.json`, `materials.json`, and `profile.json` are the actual
solver inputs; `solve/manifest.json` and `solve/out/step-0.vtkhdf` are outputs;
`result.json` records that run's measured warpage and common analytic reference.
Temporary workspaces are removed after the test, including assertion failures;
commands, solver output, mesh counts, values and errors are printed to CTest.

| Directory | nx,ny,nz | profile.ranks |
|---|---|---|
| nx4-ny4-nz2-ranks1 | 4,4,2 | 1 |
| nx8-ny8-nz4-ranks1 | 8,8,4 | 1 |
| nx16-ny16-nz8-ranks1 | 16,16,8 | 1 |
| nx8-ny8-nz4-ranks2 | 8,8,4 | 2 |

Rank 2 uses the first available mpiexec/mpirun with `-n 2`. Only absence of
both launchers skips the rank comparison; no rank-2 result is then fabricated.
All other launcher/solver failures fail the test, including exit 13.

## Boundary-condition evidence

Inspection of `apps/chipcae-solve/src/pipeline.cpp:647` shows that, with
`inputs.fixed.empty()`, only vertices belonging to regular cells enter the
candidate set. The first vertex with same-plane positive-x and positive-y
partners becomes A; scanning retains the last qualifying B and C. The
`GetVertexVDofs` loop constrains A ux/uy/uz, B uy/uz, C uz, sorts and checks
exactly six DOFs, and emits `3-2-1 constrained_dofs=6 (3+2+1)` in `solve/events.bin` (required by
the test). `mesh/background/src/tensor_grid.cpp` orders node columns by Morton
order and z ascending. For this full square (all regular cells), selection is
A=(0,0,0), B=(1,0,0), C=(0,1,0), for every tested grid and both rank counts.
The pipeline builds the same global mesh on each rank before this selection.
These selected points agree with the brief's axis and component requirements.

The test asserts that the case has exactly `constraints: {"rigid":"3-2-1"}`
and only the uniform temperature load. There are no fixed, symmetry, edge,
or surface displacement conditions or mechanical loads. In the inspected
pipeline the alternative boundary-marker constraint branch executes only
when `inputs.fixed` is nonempty. The mechanical RHS contains ThermalForce;
no mechanical surface integrator is added for this case. All faces therefore
have natural zero traction. best_fit_plane is postprocessing only.

## Acceptance records

Numerical records are added below after actual runs. Accuracy is <=3% on the
finest grid; monotonicity allows 0.001 absolute error plateau; observed orders
are diagnostic; MPI relative agreement is <=1e-8. Thresholds remain fixed.

2026-11-18, Release, MFEM/MPI/HDF5/JSONCONS enabled, Kokkos core backend
OFF (material dependency from `/workspace/deps/kokkos`). MPI launcher:
`/workspace/tools/env/bin/mpiexec`. Environment for the root-run box:
`PATH=/workspace/tasks/shim:/workspace/tools/env/bin:$PATH`,
`OMPI_ALLOW_RUN_AS_ROOT=1`, `OMPI_ALLOW_RUN_AS_ROOT_CONFIRM=1`,
`OPENBLAS_NUM_THREADS=1`, `OMP_NUM_THREADS=1`.

Normal command:
`ctest --test-dir /workspace/build-v-int-bimetal-release -R '^bimetal_timoshenko$' -V`
passed (exit 0), all four solver exits 0. Actual manifest mesh summaries matched
the directory index. All four result.json files were written. W_ref =
0.0090909090909090887 m. 实测:

| Run | W_h (m), 实测 | Relative error |
|---|---|---|
| nx4-ny4-nz2-ranks1 | 0.0090829278712031194 | 0.00087793416765662873 |
| nx8-ny8-nz4-ranks1 | 0.0090861870548405738 | 0.00051942396753663922 |
| nx16-ny16-nz8-ranks1 | 0.0090875528584402569 | 0.00036918557157150655 |
| nx8-ny8-nz4-ranks2 | 0.0090861870548405738 | 0.00051942396753663922 |

Observed p = 0.75720017743645762 and 0.49256641415483837 (diagnostic).
MPI relative difference = 0. Full command, per-run command arrays, exits,
stdout/stderr and errors: `/workspace/tasks/logs/v-int-bimetal-acceptance.log`.

Negative validation used the same CTest command, temporarily changing only
this test working copy's W_ref expression to multiply by 10. All four solves
completed (each exit 0), mesh/artifact/event checks passed, and four result.json
files were written. CTest exit 8, specifically:

```text
AssertionError: 3% accuracy assertion failed: all rank-1 errors: e_4=0.9000877934167657, e_8=0.90005194239675368, e_16=0.90003691855715706
```

MPI difference remained 0. Full command, exits and error log:
`/workspace/tasks/logs/v-int-bimetal-negative-final.log`.
The temporary multiplier was restored in a finally block and is not committed.
No production behavior was changed. The baseline already satisfies these
acceptance assertions; a normal failing accuracy result would be inaccurate.

Restored-reference rerun: same CTest command, exit 0 (56.05 s); all four
solves completed, values and errors identical to the normal table above.
Full command, solver exits, stdout/stderr, mesh summaries and errors:
`/workspace/tasks/logs/v-int-bimetal-restored-final.log`.

The initial Kokkos-enabled configuration aborted before comparison with
`Kokkos::OpenMP ... OpenMP is not initialized`. A subsequent test-authoring
check looked for the six-DOF message on stdout; inspection confirmed the
message belongs to events.bin, and the test now checks that artifact.
Neither early failure is counted as numerical negative validation. The
completed acceptance and negative records above use the corrected test and
Kokkos-OFF configuration.

Final Release suite:
`ctest --test-dir /workspace/build-v-int-bimetal-release --output-on-failure -j2`
returned 0: 172 tests, zero failures, nine expected optional-backend/placeholder
skips, total 1006.15 s. Bimetal passed in 58.77 s during this suite.
Full output: `/workspace/tasks/logs/v-int-bimetal-all-final.log`.
No C/C++ files were added or changed; clang-format 23.1.5 has no applicable
files here. Python syntax and `git diff --check` are clean.
