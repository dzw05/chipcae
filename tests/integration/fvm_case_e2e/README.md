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
main a6dd6c8 verbatim. The board compares maximum vertex `PointData/T` rise
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

Local Release measurements (实测, November 21, synthetic inputs, MFEM with
MPI/PETSc), after merging main a6dd6c8 and aligning the test oracles with I1 1.2.
The completion run measures CLI/direct-library equivalence as follows:

| Fixture | CLI PV (m) | Direct PV (m) | Relative PV difference | CLI maximum U (m) | Direct maximum U (m) | Relative U difference |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Board | 1.3390363596270016e-06 | 1.3390363596148202e-06 | 9.0971335357950544e-12 | 8.9774886012461824e-07 | 8.9774886011437316e-07 | 1.1411961307277355e-11 |
| Uniform bilayer | 0.0017444475179898623 | 0.0017444475179896589 | 1.1659641546148618e-13 | 0.0012533119660306606 | 0.0012533119660305598 | 8.0451479578942537e-14 |
| Prescribed bilayer | 0.0024422265251854217 | 0.0024422265251855656 | 5.8955239008852762e-14 | 0.0016868846951421761 | 0.0016868846951422548 | 4.6661800862334436e-14 |
| Cut bilayer | 0.0017470475538252004 | 0.0017470475538251566 | 2.5071880655171928e-14 | 0.0012549687104281072 | 0.0012549687104281013 | 4.6652093257564498e-15 |

The maximum measured CLI/direct relative difference, including both cut output
modes, is 1.1411961307277355e-11, below 1e-8. All four FEM curvature-sign
checks pass. FEM comparisons remain accuracy diagnostics for
`solve-elastic-accuracy`; the complete original PV <= 0.10 assertion is retained.
These results establish the integration equivalence, without establishing FEM
accuracy agreement.

| Fixture | FVM PV (m) | FEM PV (m) | Relative PV difference | FVM maximum U (m) | FEM maximum U (m) | Relative U difference |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Board | 1.3390363596270016e-06 | 7.6382250699617392e-06 | 0.82469273327740411 | 8.9774886012461824e-07 | 7.5633328348133282e-06 | 0.88130247871779976 |
| Uniform bilayer | 0.0017444475179898623 | 0.0019414505357316298 | 0.1014720767364426 | 0.0012533119660306606 | 0.0020458574104428601 | 0.38739036277246702 |
| Prescribed bilayer | 0.0024422265251854217 | 0.0027174470566019389 | 0.10127907763570919 | 0.0016868846951421761 | 0.0028296925696029672 | 0.40386290960969584 |
| Cut bilayer | 0.0017470475538252004 | 0.0022505694928693014 | 0.22373090039630353 | 0.0012549687104281072 | 0.0017169407779141516 | 0.26906697856362721 |

| Fixture | min_dn_ratio | min_dn_gid | gradient_corrections | max_identity_defect |
| --- | ---: | ---: | ---: | ---: |
| Board | 1 | 0 | 0 | 0 |
| Uniform bilayer | 1 | 0 | 0 | 0 |
| Prescribed bilayer | 1 | 0 | 0 | 0 |
| Cut bilayer | 0.79262398910460019 | 8 | 0 | 4.4408920985006262e-16 |

The diagnostics are read by the temporary direct-library replay probe.
Production `FVM_ELASTIC` retains exactly its four required keys.

| Fixture / process | Wall seconds | Measured peak RSS (KiB) |
| --- | ---: | ---: |
| Board / fvm1 | 1.901692 | 54844 |
| Board / fvm2 | 2.234879 | 54844 |
| Board / fem1 | 1.766255 | 65528 |
| Uniform bilayer / strip-fvm | 1.592581 | 99460 |
| Uniform bilayer / strip-fem | 1.958119 | 107764 |
| Prescribed bilayer / prescribed-fvm | 1.654005 | 99460 |
| Prescribed bilayer / prescribed-fem | 2.196742 | 110544 |
| Cut bilayer / cut-elastic-polyhedron | 1.722992 | 54844 |
| Cut bilayer / cut-elastic-hex | 1.741614 | 54844 |
| Cut bilayer / cut-fem | 2.466877 | 125204 |

RSS is the wait4 process/child maximum, including the MPI launcher, rather
than a sum across ranks; it can include memory inherited at fork. Timings are
observations under shared load, not performance gates.

Fresh completion verification (实测): the branch e2e passes all 55 methods in
382.303 unittest seconds within the 11/11 passing FVM/elastic group
(466.93 total CTest seconds). This includes all 14 frozen I14 fixtures,
caller order, independent materials and temperature oracles, reconstruction,
fixed/free edges, zero rise, MPI, STOP, error mapping and both cut output modes.
The board vertex-temperature rise relative difference is 0.014728203110031465,
below the unchanged 0.02 gate. Matching-main a6dd6c8 passes its original 22-method
e2e in 158.923 unittest seconds (159.16 CTest seconds);
it does not contain this branch's added adapter tests. The integration selector
passes 2/2 tests in 11.56 CTest seconds; no separate
`demo_gate`-named CTest is registered, while the demo method runs within e2e.

Before the oracle correction, branch 627f255 failed exactly two stiffness checks
(9/36 entries each) in its 55-method e2e. The independent references still used
the pre-I1-1.2 Ez/alpha_z/kz rules. Correcting both mechanical oracles and replay
conductivity resolved those failures without changing production or tolerances.
The older thermal/snapshot failure reports predate the main merge and are
superseded by the passing runs above.

The earlier `^solve_` selector on the same implementation and oracles recorded
93/97 active passes, four failures and ten preexisting disabled tests. Its
outside-scope results were resolved or classified under the 20:40 ruling:

| Outside-scope test | Selector result | Isolated / matching-main evidence | Classification |
| --- | --- | --- | --- |
| solve_ghost_scan | Timeout at 300.11 s | Final isolated branch PASS 203.60 s; main PASS 239.89 s | 资源争用，-j1 通过 |
| solve_column_gmg_fcbga | MPI exit 137 | Isolated branch PASS 118.45 s | 资源争用，-j1 通过 |
| solve_column_gmg_fcbga_scaling | Child SIGKILL at 89.10 s | Matching-main child SIGKILL at 64.39 s | 基线红/环境项 |
| solve_fem_thermal_bow | C-0/A residual assertion | Main has the same residual 9.5759468232801005e-13 and limit 2.5499999995493557e-15 | 基线红 |

Fresh evidence is retained in local task logs with prefix
`fvm-wire-elastic-completion-current-`: `fvm-elastic.log`, `main-e2e.log`,
`integration.log`, `metrics.json`, `results.json` and `audit.json`. The earlier
RED, broader solve selector, isolated and matching-main evidence use prefix
`fvm-wire-elastic-tdd-current-`. No numerical tests or solver options change in
this documentation pass, and the complete CTest suite is left to the pipeline.

Green `ci-ok` and the specified nightly/Mac full-deps runner's green elastic,
bimetal and np2 logs remain merge requirements. Local full-dependency green
logs do not establish that runner-origin gate. These external gates are
未执行/未验证; no workflow is dispatched or result inferred. Real-board inputs,
commands, logs and hashes remain local and outside commits.
