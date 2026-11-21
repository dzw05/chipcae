# FVM case CLI acceptance

`fvm_fvm_case_e2e` selects `fvm.conduction` / `fvm.solid` through
`physics[].solver`, without adding CLI arguments. It is registered with labels
`integration;numerical;fvm`, timeout 1200 seconds and skip return code 77.
It exercises the real loader, mesh, thermal and elastic solvers, results
writer and event stream using temporary synthetic fixtures. CMake supplies
the configured executables and MPI launcher. Missing executables or numpy/h5py
return 77; configured command failures fail acceptance. Schema validation
alone skips if jsonschema is missing.

The mechanics gates require solved, finite, nonzero U and positive warpage
for each nonzero acceptance case. Summary methods must equal
`FVM top-face centroids; least-squares plane removal` and `FVM cell centers`.
The one rank-0 level-1 `FVM_ELASTIC ` record has exactly four unique keys:
`iterations`, `residual`, `warpage_pv`, `U_mag_max`. Iterations must be a
nonnegative integer and all other values finite and nonnegative. The log,
manifest and final `fvm.solid`/`U` residual agree to relative tolerance 1e-12.
The former displacement fallback is forbidden in adapter source and output.

Under the November 21 18:02 ruling (`handoff/rulings/fvm-wire-elastic.md`),
board, uniform bilayer, layer-prescribed-temperature bilayer and cut cases
compare CLI PV and maximum displacement with direct `solve_elastic` replay,
using relative tolerance 1e-8. The temporary replay executable uses only public
I14 headers; an independent NumPy oracle checks its captured inputs, including
all 36 stiffness entries, three CTEs, copper fractions, dT and supports.
FVM and FEM must retain the same signed top quadratic curvature after plane
removal. The complete original `compare_mechanics` function, including its
unchanged PV <= 0.10 assertion, remains in the helper for
`solve-elastic-accuracy` and is not called by this card. Tests print both
PVs, both U maxima and their relative differences. Each process reports wall
time and measured peak child RSS where wait4 is available; MPI RSS is a
maximum, not a rank sum. These measurements are not performance thresholds.
Zero temperature rise retains normal methods and requires PV <= 1e-14 times
characteristic length and all displacement components <= 1e-14 m. MPI np=1/2
aligns by parent GID and vertex coordinates, using global relative U and PV
tolerances of 1e-8. Rigid321, fixed bottom and a nonfirst fixed layer exercise
support mapping and prescribed zero vertices (<= 1e-14 m), including fixed/free
edges.

Nonconvergence must exit 1, preserve the complete original I14 error reason,
and publish no successful mechanics output or manifest step. I14 1.0 carries
no structured failure residual; the ruling removes the four np=1/2 failure
residual requirements. Successful residual checks remain. Demo failures are
checked for original-reason passthrough without requiring convergence.
The pipeline error fixture exercises `run_fvm` with five opaque I14 errors
at np=1 and np=2: INPUT_INVALID, INTERNAL, double-prefixed INTERNAL, numerical
failure and the obsolete embedded-face diagnostic. It requires unchanged
reason text, the existing exit code, one rank-0 error and terminal exit,
preserved thermal success and no successful mechanics output.
STOP fixtures cover prequeued commands and a FIFO that queues STOP after the
first control acknowledgment. The second checkpoint must acknowledge it once,
end with `user_stop`, and publish no output. The FIFO tests checkpoint ordering,
not cancellation inside the elastic library.

Cut mechanics tests exercise both polyhedron and `--cut-output hex` output.
Frozen I14 1.0 supports embedded faces and cut CVs. Both output modes must
succeed, publish finite nonzero cut/EB U, and agree on summaries and EB
coordinates/U. A direct adapter regression requires the obsolete embedded-face
rejection reason to use ordinary INPUT_INVALID handling (exit 14). The adapter
passes `is_cut` in caller order, including merged CVs, and converts grid side 6 to
I14's embedded side -1. No uncut substitution is used.

All previous loader, schema, thermal, strict energy ledger, Wiener, MPI,
physical volume, copper diagnostics, negative input and FEM snapshot
assertions remain. The merge retains four additional upstream tests for
FVM-only volume selection, loader error precedence, fixed-temperature
roundoff and split-layer convection. Frozen FEM snapshots predate upstream
changes to copper fields and conductivity parsing; they are preserved.

The decoder's negative fixtures reject wrong levels/ranks, missing or repeated
records, duplicate/missing/extra keys, invalid numeric types, nonfinite values,
CRC errors, truncated frames and trailing payload bytes. Partition fixtures
exercise reordered ranks and deliberately perturbed U/T. Curvature fixtures
verify rigid translation/tilt invariance.

Use plain shared build-slot wrappers with at most two build jobs:

```sh
cmake --build "$build" -j2
ctest --test-dir "$build" -N -L fvm
ctest --test-dir "$build" -R '^fvm_fvm_case_e2e$' --output-on-failure
ctest --test-dir "$build" -R 'fvm_|elastic' --output-on-failure
ctest --test-dir "$build" -L integration -R 'f1130|demo_gate' --output-on-failure
ctest --test-dir "$build" -R '^solve_' --output-on-failure -j1
```

Supply local dependency prefixes as needed and retain the shared shim's PATH
precedence. Full numerical builds require MFEM with MPI and PETSc, ECAD,
jsoncons and HDF5. Bare configuration keeps those dependencies optional.
During iteration, use the named selectors above or the task's related-test
selector. The pipeline runs the complete suite after delivery.

Local Release measurements (实测, November 21, synthetic inputs, MFEM with
MPI/PETSc). These values come from the current 55-method implementation run:

| Fixture | FVM PV (m) | FEM/direct PV (m) | Relative PV difference | FVM maximum U (m) | FEM maximum U (m) | Relative U difference |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Imported board | 1.3821524030478399e-06 | 7.6705217582044357e-06 | 0.81980985823168007 | 9.2238241640278916e-07 | 7.5643159714428214e-06 | 0.87806135810759189 |
| Uniform bilayer | 0.0017444475179898399 | 0.0019414505357316298 | 0.1014720767364541 | 0.0012533119660306468 | 0.0020458574104428601 | 0.3873903627724738 |
| Prescribed-temperature bilayer | 0.0024422265251855396 | 0.0027174470566019389 | 0.10127907763566578 | 0.0016868846951422548 | 0.0028296925696029672 | 0.40386290960966803 |
| Cut bilayer | 0.0017470475538252663 | 0.0022505694928693014 | 0.22373090039627422 | 0.0012549687104280432 | 0.0017169407779141516 | 0.26906697856366446 |

FEM PV differences are diagnostics for `solve-elastic-accuracy`; curvature-sign
checks pass in all four cases. They do not change the original accuracy bound.
The maximum CLI/direct replay relative difference is 8.6075487040211334e-12,
below the unchanged 1e-8 gate. Both cut output modes pass with nonzero U.

| Fixture | min_dn_ratio | min_dn_gid | gradient_corrections | max_identity_defect |
| --- | ---: | ---: | ---: | ---: |
| Imported board | 1.0 | 0 | 0 | 0.0 |
| Uniform bilayer | 1.0 | 0 | 0 | 0.0 |
| Prescribed-temperature bilayer | 1.0 | 0 | 0 | 0.0 |
| Cut bilayer | 0.7926239891046002 | 8 | 0 | 4.440892098500626e-16 |

The diagnostics are read by the temporary replay probe. Production
`FVM_ELASTIC` retains exactly its four required keys.

| Fixture / process | Wall seconds | Measured peak RSS (KiB) |
| --- | ---: | ---: |
| Imported board / fvm1 | 1.727795 | 60804 |
| Imported board / fvm2 | 1.958002 | 60804 |
| Imported board / fem1 | 1.678256 | 65400 |
| Uniform bilayer / strip-fvm | 1.598698 | 94788 |
| Uniform bilayer / strip-fem | 1.939698 | 107888 |
| Prescribed-temperature bilayer / prescribed-fvm | 1.587796 | 94788 |
| Prescribed-temperature bilayer / prescribed-fem | 2.088682 | 110452 |
| Cut bilayer / cut-elastic-polyhedron | 1.624042 | 65284 |
| Cut bilayer / cut-elastic-hex | 1.608626 | 65284 |
| Cut bilayer / cut-fem | 2.318108 | 125476 |

RSS is the wait4 process/child maximum, including the MPI launcher, rather
than a sum across ranks; it can include memory inherited at fork. Timings are
observations under shared load, not performance gates.

The current focused e2e runs 55 unittest methods in 328.813 seconds (329.05
CTest seconds), with five assertion failures and one error. All 11 adapter
methods pass, including the 14-case frozen I14 checker. Caller order,
embedded-top selection, material and
temperature oracles, reconstruction, fixed/free edges, zero rise, MPI, STOP,
all ten pipeline error subcases and all four direct-library comparisons pass.

The five remaining assertions are unchanged:

- Board maximum CV temperature is 321.92657327984955 K versus FEM
  323.98160735460056 K, with reference 298.15 K. The temperature-rise relative
  difference is 0.079555021355843233, above the original 0.01 gate. `T_max`
  retains the required maximum-CV semantics; no rescaling is applied.
- Four FEM snapshots (`default`, `sources`, `solid`, `copper`) predate default
  `copper_fraction`/`conductivity_diag` output fields. The solid-only snapshot
  also predates conductivity loading (actual k=2/4, snapshot k=0/0).
  Snapshot values and loader behavior remain unchanged in this card.

The additional error is in
`test_solid_only_nonconvergence_preserves_library_reason_without_success`
at np=2: the temporary snapshot probe exits 0, then `direct_elastic` raises
`JSONDecodeError: Extra data` while decoding its stdout. That subcase does not
complete its original-reason comparison. The cause is not established or
classified as outside scope. No assertion or decoder is relaxed.

The implementation pass stops after reproducing the thermal and loader
blockers, as required by the task. No numerical retries are made. Release
configure/build, bare configure and `ctest -N -L fvm` pass; the latter lists
15 tests, including the required elastic, bimetal and np2 registrations.

The 20:40 ruling requires outside-scope failures to pass an individual `-j1`
rerun or reproduce on starting main with the same build options. The current
implementation pass stops at the e2e blockers under the task's stop rule;
broader regressions are not rerun in this pass.

Green `ci-ok` and the specified nightly/Mac full-deps runner's green elastic,
bimetal and np2 logs remain merge requirements. Local full-dependency green
logs do not establish that runner-origin gate. These external gates are
未执行 in this pass. No remote workflow is dispatched or result inferred.
Real-board inputs, commands, logs and hashes remain local and outside commits.
