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
The material oracle independently inverts the engineering compliance matrix in
`(xx, yy, zz, yz, xz, xy)` order using NumPy. It follows I1 1.2: all normal
moduli and conductivities are parallel mixtures, Poisson ratios are fraction
weighted, xy shear follows E/[2(1+nu)], and xz/yz shear is harmonic. In-plane CTE
uses biaxial-modulus weights; through-thickness CTE uses E weights. Fractions
0, 0.25 and 0.75 retain stiffness tolerances rtol=1e-12/atol=1e-5 and CTE
tolerances rtol=1e-12/atol=1e-16; replay conductivity uses
rtol=1e-12/atol=1e-14. Neither oracle calls the production mixture helper.
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
roundoff and split-layer convection. `FEM_SNAPSHOTS` and `probe` match merged
main b1cf5fa verbatim. The board compares maximum vertex `PointData/T` rise
with the unchanged upstream temporary 0.02 gate and its restoration comment;
the demo retains its 0.01 gate. Summary `T_max` still means maximum CV temperature.

The decoder's negative fixtures reject wrong levels/ranks, missing or repeated
records, duplicate/missing/extra keys, invalid numeric types, nonfinite values,
CRC errors, truncated frames and trailing payload bytes. Partition fixtures
exercise reordered ranks and deliberately perturbed U/T. Curvature fixtures
verify rigid translation/tilt invariance.

Use plain shared build-slot wrappers with at most two build jobs:

```sh
cmake --build "$build" -j2
ctest --test-dir "$build" -N -L fvm
ctest --test-dir "$build" -j1 -R '^fvm_fvm_case_e2e$' --output-on-failure
ctest --test-dir "$build" -j1 -R 'fvm_|elastic' --output-on-failure
ctest --test-dir "$build" -j1 -L integration -R 'f1130|demo_gate' --output-on-failure
ctest --test-dir "$build" -R '^solve_' --output-on-failure -j1
```

Supply local dependency prefixes as needed and retain the shared shim's PATH
precedence. Full numerical builds require MFEM with MPI and PETSc, ECAD,
jsoncons and HDF5. Bare configuration keeps those dependencies optional.
During iteration, use the named selectors above or the task's related-test
selector. The pipeline runs the complete suite after delivery.

Local Release measurements (实测, November 22, synthetic inputs, MFEM with
MPI/PETSc), at branch 6f6eb7a after merging main b1cf5fa. I1 1.2 oracle
correction 829a26b and the production adapter are unchanged. The numerical gate
compares CLI summaries with direct public-I14 replay:

| Fixture | CLI PV (m) | Direct PV (m) | Relative PV difference | CLI maximum U (m) | Direct maximum U (m) | Relative U difference |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Board | 1.3390363596305975e-06 | 1.3390363596321188e-06 | 1.1360939994838312e-12 | 8.9774886012789001e-07 | 8.9774886012865869e-07 | 8.5623322264437705e-13 |
| Uniform bilayer | 0.0017444475179894906 | 0.0017444475179894832 | 4.2263093024423551e-15 | 0.0012533119660304633 | 0.0012533119660304583 | 3.9793204953028564e-15 |
| Prescribed bilayer | 0.0024422265251852707 | 0.0024422265251854893 | 8.9498314639947958e-14 | 0.0016868846951421125 | 0.0016868846951422305 | 6.992842884052423e-14 |
| Cut bilayer | 0.0017470475538247138 | 0.0017470475538247849 | 4.0710776509396741e-14 | 0.0012549687104278643 | 0.0012549687104278218 | 3.3865963994387696e-14 |

The maximum CLI/direct relative difference, including both cut output modes,
is 1.1360939994838312e-12, below 1e-8. All four FEM curvature-sign checks pass.
FEM PV/U differences remain diagnostics for `solve-elastic-accuracy`; its complete
original PV <= 0.10 assertion is retained. These results establish wiring
equivalence; FEM accuracy agreement remains deferred.

| Fixture | FVM PV (m) | FEM PV (m) | Relative PV difference | FVM maximum U (m) | FEM maximum U (m) | Relative U difference |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Board | 1.3390363596305975e-06 | 7.6382250700526953e-06 | 0.82469273327902082 | 8.9774886012789001e-07 | 7.5633328349347987e-06 | 0.88130247871927359 |
| Uniform bilayer | 0.0017444475179894906 | 0.0019414505357316298 | 0.10147207673663404 | 0.0012533119660304633 | 0.0020458574104428601 | 0.38739036277256345 |
| Prescribed bilayer | 0.0024422265251852707 | 0.0027174470566019389 | 0.10127907763576473 | 0.0016868846951421125 | 0.0028296925696029672 | 0.40386290960971832 |
| Cut bilayer | 0.0017470475538247138 | 0.0022505694928693014 | 0.22373090039651972 | 0.0012549687104278643 | 0.0017169407779141516 | 0.26906697856376866 |

| Fixture | min_dn_ratio | min_dn_gid | gradient_corrections | max_identity_defect |
| --- | ---: | ---: | ---: | ---: |
| Board | 1 | 0 | 0 | 0 |
| Uniform bilayer | 1 | 0 | 0 | 0 |
| Prescribed bilayer | 1 | 0 | 0 | 0 |
| Cut bilayer | 0.79262398910460019 | 8 | 0 | 4.4408920985006262e-16 |

The temporary direct-library probe reads these diagnostics. Production
`FVM_ELASTIC` retains exactly its four required keys.

| Fixture / process | Wall seconds | Measured peak RSS (KiB) |
| --- | ---: | ---: |
| Board / fvm1 | 1.858702 | 55268 |
| Board / fvm2 | 2.059942 | 55268 |
| Board / fem1 | 2.202291 | 64968 |
| Uniform bilayer / strip-fvm | 1.598318 | 95848 |
| Uniform bilayer / strip-fem | 2.040973 | 107664 |
| Prescribed bilayer / prescribed-fvm | 2.255176 | 95848 |
| Prescribed bilayer / prescribed-fem | 2.390781 | 109880 |
| Cut bilayer / cut-elastic-polyhedron | 1.789898 | 55268 |
| Cut bilayer / cut-elastic-hex | 1.662002 | 55268 |
| Cut bilayer / cut-fem | 2.597915 | 125112 |

RSS is the wait4 process/child maximum, including the MPI launcher, rather
than a sum across ranks; it can include memory inherited at fork. Timings reflect
shared load and are observations, not performance gates.

Current-HEAD verification (实测): all 55 e2e methods pass in 366.994 unittest
seconds within the 11/11 passing FVM/elastic group (432.33 CTest
seconds). This covers all 14 I14 fixtures, independent material and temperature
oracles, reconstruction, fixed/free edges, zero rise, MPI, STOP, error mapping
and both cut output modes. The board vertex-temperature rise relative difference
is 0.014728203110031465, below the unchanged 0.02 gate.
Matching-baseline main a6dd6c8 passes its original 22 e2e methods in
365.132 unittest seconds (365.64 CTest seconds); that suite
does not contain the branch's added adapter tests. Both builds use matching
Release dependency options. The integration selector passes 2/2 in
8.98 CTest seconds. No separate `demo_gate` CTest is registered;
the demo method runs in e2e.

The original unchanged-branch RED evidence is preserved: 627f255 failed exactly
two stiffness checks (9/36 entries each) in 55 methods. Both independent
references still used pre-I1-1.2 Ez/alpha_z/kz rules. Commit 829a26b corrected
those formulas without changing production behavior or tolerances. This pass
inherits that correction and does not recreate a failing commit.

The fresh `^solve_` selector records 98 passes,
1 failure, 0 skips and
10 preexisting disabled tests in 1642.83 CTest seconds.

Failures in this selector: `solve_column_gmg_fcbga_scaling`. See the local task report for causes and scope classification. No out-of-scope source is changed or test retried.

Scaling's fine-GMG child returns -9 (SIGKILL) in 136.78 seconds. The historical
matching-main a6dd6c8 run with the same options also records SIGKILL
(`fvm-wire-elastic-tdd-current-main-fcbga-scaling.log`), classified as 基线红/环境项
under the ruling. This pass reports `BLOCKED-OUT-OF-SCOPE` for the broader
regression result and makes no attempt to change that module. Ghost scan and
FCBGA reference pass this run. The upstream thermal-bow correction is included
via merge 76277ae; `solve_fem_thermal_bow` passes in 18.53 seconds.

Evidence files in the local task log directory use prefix
`fvm-wire-elastic-resume-`: `build.log`, `registered.log`, `main-build.log`,
`fvm-elastic.log`, `main-e2e.log`, `integration.log`, `solve.log`, `metrics.json`,
`results.json`, `statuses.json` and `audit.json`. Original RED and broader
historical evidence retain prefix `fvm-wire-elastic-tdd-current-`.
No numerical source, assertion, solver option, skip, xfail or timeout is changed.
The complete CTest suite is left to the pipeline.

Green `ci-ok` and nightly/Mac full-deps runner green elastic, bimetal and np2
logs remain merge requirements. They are 未执行/未验证 for these local commits;
local green tests do not satisfy runner provenance. No push, PR or workflow
dispatch is performed. Real-board inputs, commands, logs and hashes stay local
and outside commits.
