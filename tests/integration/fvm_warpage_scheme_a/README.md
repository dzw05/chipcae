# INT-a scheme A decision library and acceptance tests

`tools/vv/fvm_warpage_scheme_a.py` implements the INT-a decision library without
changing solver behavior. The preceding tests-only commit established the red
acceptance evidence. Each unittest loads the module in `setUp`; its absence is
an explicit assertion failure for every case, rather than a collection error.
Missing numpy or h5py exits 77, which fails this CTest registration. There are no
skips, expected failures, or fallback evaluators.

All inputs are generated in temporary directories by `synthetic_inputs.py`.
Nothing comes from a solver, contract fixture, vendor, or customer case. The
serial HDF files use the existing `VTKHDF/hex` and `VTKHDF/cut` layout, with a
single count entry per block, ordinary hex connectivity, and closed extruded
polygon polyhedra with face and polyhedron offsets. Empty blocks still have a
single zero count. The manifest points to `out/first.vtkhdf` via `steps[0].file`;
the first step is deliberately numbered 7 rather than zero. Requested dimensions
come independently from `case.json`.

The tests catch wrong plane subtraction, node/face sampling confusion, wrong
refinement semantics, vertex averages substituted for polygon area centroids,
local instead of global top selection, inclusive instead of strict z thresholds,
malformed topology, absent callback validation, wrong PV denominators, relaxed
boundaries, omitted legacy gates, ignored curvature/deformation failures, and
cumulative rather than per-child resource accounting.

## Analytic expectations

- Plane plus quadratic: symmetric endpoint/midpoint samples give
  `abs(kappa)*(xmax-xmin)^2/8`, relative tolerance `1e-12`; pure plane PV is at
  most `1e-15`. Node/centroid PV ratios are `1-2/nx` for even nx and
  `(nx-1)/(nx+1)` for odd nx, with no inserted samples.
- Hex vertices and expected centroids use dyadic spacings. The required centroid
  equality is bitwise, including the `(24,8,L=1)` to `(48,16)` case.
- The asymmetric top trapezoid `(0,0),(4,0),(2,2),(0,2)` has area 6 and first
  moments `28/3,16/3`, hence centroid `(14/9,8/9)`. Translated independent prisms
  test disconnected faces and reversed winding. These expectations do not use a
  polygon algorithm under test.
- The same-point callback evaluates `kappa*(x*x+y*y)/2+a+b*x+c*y`, with
  `kappa=.01`, on the reconstructed face samples. An analytic tangent subtracts
  the fitted plane up to an irrelevant constant. On the chosen 8 by 4 grid its
  PV is `.00625`, derived from squared distances to the symmetry center.
  The expected same-point and nodal errors are `-.04` and `-.25`.
- FEM convergence cases set `coarse=fine*(1-d)`, using `d=.9999e-3` or
  `1.0001e-3` independently for same-point, nodal, and thickness differences.
  Only division by the **finer** PV passes the below-threshold samples.
- Gate limits use binary64 directly, with no comparison tolerance. The constructed
  observed orders are checked with `float.hex()` before invoking the gate.
  Power-of-two fine errors ensure targets `1.5`, `1.4999`, `1`, `.9999` are
  obtained bitwise. Absolute thresholds are inclusive. An error just above a
  fine-grid absolute limit can also violate the adjacent-grid constraint; the
  test separately requires a failing check with the specified absolute limit.
- The deformation field is a uniform expansion of a symmetric zero-mean point
  set, orthogonal to all rigid modes, plus translation and infinitesimal rotation.
  Its maximum is `.02*sqrt(5.25)`, relative tolerance `1e-10`.
- One real child touches 64 MiB and the next does almost no allocation. Their RSS
  must be respectively at least and below 60 MiB. Other cases verify wait status,
  signals, environment, cwd, inherited streams and reaping. OS memory parsing is
  tested through mocked file reads and command output on both platforms.

## Library conventions specified for INT-b

The brief leaves the Python tier mapping representation and invalid-input shape
open. These tests adopt the following internal conventions:

- `InputInvalid` is a module-local `ValueError` subclass. Invalid geometry, files,
  array shapes, callback results, and resource data raise it. Gate and reference
  functions return failed checks with nonempty reasons for invalid records.
  `require_convergence_quantity` rejects unsupported quantities with `ValueError`.
- Mesh fields and requested dimensions must be actual integers, excluding booleans;
  nx/ny are positive, L is nonnegative, and padding is zero. Missing fields are
  errors. No implicit dimensions, default evaluator, or nodal fallback is allowed.
- A gate record's unique key is `(nx,ny,kz)`. Duplicate keys fail. Gate results have
  `pass` and `checks`; each check has `name,value,limit,pass,reason`. Check order is
  deterministic and independent of input record order. The tests leave check
  names and grouping free so long as the result is stable and reasons identify
  failure. kz16 diagnostics and interpolation fields must not alter this result.
  The implementation orders strip groups by `(nx,ny)`, then kz4 before kz8;
  each record checks absolute error, deformation, then curvature, followed by
  that group's thickness order. The final checks are the `(48,16)` to `(96,32)`
  in-plane orders for kz4, then kz8. Both base strip groups and complete kz4/kz8
  pairs are mandatory; supplied nx>=96 strip groups use the same fine-grid limits.
  Every gated record supplies finite `same_point_rel`, `deformation_rel`,
  `fvm_curvature`, and `fem_curvature`, along with its integer key fields.
- FEM tiers are a list of dicts with `nx,ny,kz,nodal_pv,same_point_pv`.
  `same_point_pv` maps stable FVM `(nx,ny,kz)` tuples to positive floats, including
  kz16 keys. FEM tier keys are also `(nx,ny,kz)`; duplicates and missing required
  tiers/keys fail. `reference` maps each FVM tuple to its independently selected
  FEM tuple. Selection follows ascending adjacent kz2 tiers, regardless of list
  input order, after checking `(12,4,kz2)` against `(12,4,kz4)` for every key.
  All five prescribed FEM tiers and all FVM keys are required. Checks run in
  ascending FVM-key order, thickness before reference selection. A rejected
  intermediate pair may be refined further: the reference check reports the
  first accepted pair, or the last pair and `参考未收敛` when none qualifies.
- Signed errors remain binary64: same-point `(fvm-ref)/ref`, nodal `fvm/nodal-1`.
  Diagnostic comparison failures print `.17g`; no rounded values enter decisions.
  All PV denominators must be finite and positive; zero output is still valid for
  pure-plane `plane_pv` and pure-rigid `deformation_max`.
- The callback `(fem_run_dir,xy)->positive PV` is called exactly once with the
  reconstructed xy. The six result keys are `fvm_pv`, `fem_same_point_pv`,
  `same_point_rel`, `fem_nodal_pv`, `nodal_rel`, and `n_samples`; any `interp_indicator` or
  `interp_indicator_*` result key is forbidden. The exact FVM method is
  `FVM top-face centroids; least-squares plane removal`.
- Undefined order, including a zero finer error, is conservatively classified as
  `阶不可判`, consistent with the plan. There is no small-error exemption.
- Uncut strip sample count equals refined nx*ny. Cut samples instead follow the
  internally consistent face topology and may have disconnected top pieces;
  they need not equal nx*ny. All geometries require at least three samples.
- `plane_pv` imports `best_fit_plane` from `warpage_qoi`; `deformation_max` imports
  `_remove_rigid` from `field_compare`. Spies wrap the real numerical routines;
  none of the numeric expected values comes from a mocked result.

## Interfaces

Import from `tools.vv.fvm_warpage_scheme_a` with the repository on `sys.path`:

| Function | Input and result |
|---|---|
| `plane_pv(xy,w)` | Finite `(n,2)` samples and `(n,)` values, n>=3; float PV after shared plane removal |
| `top_face_centroids(run_dir,requested_nxny)` | Manifest, first-step HDF and mandatory requested dimensions; binary64 `(n,2)` xy |
| `same_point_record(fvm_run_dir,fem_run_dir,fem_same_point_pv)` | FVM case/manifest/HDF, FEM manifest, injected callback; the six required scalar keys listed above |
| `fem_reference_self_convergence(tiers)` | FEM tier dictionaries; `pass,reference,checks` |
| `deformation_max(points,displacement)` | Matching nonempty finite `(n,3)` arrays; maximum norm after shared rigid removal |
| `scheme_a_gate(records)` | Strip record dictionaries; `pass,checks`, retaining kz4 and deformation limits |
| `scheme_a_preconditions(nx,kz_min,same_point_rel,nodal_rel,legacy_gate)` | Caller threshold and errors; `pass,checks,reason`; only None omits the node check |
| `require_convergence_quantity(name)` | Accept `warpage_pv` or `deformation_max`; otherwise raise ValueError |
| `run_measured(argv,env,cwd)` | Inherited streams, Popen followed by wait4; `returncode,wall_s,peak_rss_mib` |
| `rss_mib(ru_maxrss,platform)` | Nonnegative peak: Linux KiB or Darwin bytes to MiB |
| `mem_available_bytes()` | Linux MemAvailable, or Darwin free+inactive+speculative pages times sysctl page size; integer bytes |

All file/topology validation finishes before samples are returned. Blocks require
one count entry, matching point/connectivity counts, monotone offsets, bounded
indices, and consistent polyhedron face references. The global top uses strict
`abs(z-z_top)<1e-14`. Hex samples follow cell order; cut samples follow face order
and signed area moments, preserving reversed winding and disconnected polygons.

Resource values are the raw per-child wait4 result, never cumulative
RUSAGE_CHILDREN or an adjusted/subtracted estimate. A child's pre-exec memory
can contribute to that peak. The real allocation probe therefore runs in a fresh
interpreter, isolating it from earlier geometry-test allocations while still
checking an actual 64 MiB child followed by an actual minimal child.

## Recorded implementation deviations

- The order-boundary fixture originally assigned only its constructed coarse
  error and left the gate's fine error at `.003`. It now assigns the constructed
  `2**-9` fine error as well, and additionally asserts that the actual supplied
  pair produces the target order bitwise. Every original assertion is retained.
- The RSS probe now uses a fresh interpreter. On this Linux box, an unisolated
  minimal child's peak equalled the parent's 64,004 KiB peak after geometry
  tests. Both original RSS bounds (high>=60 MiB and low<60 MiB) are retained.
- Undefined zero-fine orders fail with `阶不可判`; cut sample counts use internal
  face consistency rather than the uncut nx*ny rule, as specified in the plan.
- Full CTest is deferred to the pipeline by the task instruction.

## Commands and scope

Keep the supplied build-tool PATH and shared-slot wrappers. Configure and build
Release, then inspect registration and run only the named acceptance/regressions:

```sh
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCHIPCAE_BUILD_TESTS=ON
cmake --build build-release --parallel 2
ctest --test-dir build-release -N -R '^vv_fvm_warpage_scheme_a_unit$'
ctest --test-dir build-release -R '^(vv_fvm_warpage_scheme_a_unit|warpage_qoi_analytic|vv_result_schema)$' --output-on-failure
ctest --test-dir build-release -V -R '^vv_fvm_warpage_scheme_a_unit$'
```

The new test has label `vv`, timeout 120 seconds, repository-root working
directory, and **no** `SKIP_RETURN_CODE`. Missing dependencies fail acceptance.
No full CTest suite is run here; the pipeline owns the full Release run.
No C/C++ file is changed, so clang-format has no applicable source files.

INT-a implements the gate library and unit tests. Q2 point values belong to
`v-int-fem-q2-point-ref`; this directory implements no Q2 shape evaluator. INT-b
(`v-int-fvm-warpage-gate-a2`) injects that evaluator and runs the actual scheme A
assessment. These synthetic tests provide no evidence that the solver passes
that assessment. No solver run, Q2 evaluator, or evaluator integration is part
of this deliverable.
