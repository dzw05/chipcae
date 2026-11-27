# INT-a scheme A acceptance tests

This directory contains the **test-first red pass only**. It specifies the future
`tools/vv/fvm_warpage_scheme_a.py` library without supplying an implementation or
changing solver behavior. Each unittest loads that module in `setUp`; its absence
is an explicit assertion failure for every case, rather than a collection error.
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
- FEM tiers are a list of dicts with `nx,ny,kz,nodal_pv,same_point_pv`.
  `same_point_pv` maps stable FVM `(nx,ny,kz)` tuples to positive floats, including
  kz16 keys. FEM tier keys are also `(nx,ny,kz)`; duplicates and missing required
  tiers/keys fail. `reference` maps each FVM tuple to its independently selected
  FEM tuple. Selection follows ascending adjacent kz2 tiers, regardless of list
  input order, after checking `(12,4,kz2)` against `(12,4,kz4)` for every key.
- Signed errors remain binary64: same-point `(fvm-ref)/ref`, nodal `fvm/nodal-1`.
  Diagnostic comparison failures print `.17g`; no rounded values enter decisions.
  All PV denominators must be finite and positive; zero output is still valid for
  pure-plane `plane_pv` and pure-rigid `deformation_max`.
- The callback `(fem_run_dir,xy)->positive PV` is called exactly once with the
  reconstructed xy. Six result keys are required; any `interp_indicator` or
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

## Commands and scope

Keep the supplied build-tool PATH and shared-slot wrappers. Configure and build
Release, then inspect registration and run only the named acceptance/regressions:

```sh
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCHIPCAE_BUILD_TESTS=ON
cmake --build build-release --parallel 2
ctest --test-dir build-release -N -R '^vv_fvm_warpage_scheme_a_unit$'
ctest --test-dir build-release -R '^(vv_fvm_warpage_scheme_a_unit|warpage_qoi_analytic|vv_result_schema)$' --output-on-failure
```

The new test has label `vv`, timeout 120 seconds, repository-root working
directory, and **no** `SKIP_RETURN_CODE`. This pass must leave it red. No full
CTest suite is run in this pass; the pipeline owns the later full Release run.
No C/C++ file is changed, so clang-format has no applicable source files.

INT-a specifies the gate library and unit tests. Q2 point values belong to
`v-int-fem-q2-point-ref`; this directory implements no Q2 shape evaluator. INT-b
(`v-int-fvm-warpage-gate-a2`) injects that evaluator and runs the actual scheme A
assessment. These synthetic tests provide no evidence that the solver passes
that assessment. No PR, push, solver run, or green implementation is part of
this test-first deliverable.
