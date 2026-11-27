# FEM Q2 point reference acceptance

All inputs are synthetic analytic fields generated in temporary directories by
`q2_synth.py`. No solver, stored fixture, real case, or optional backend is used.
Python's standard library and NumPy generate the tests; the future tool reuses
the existing plane fit and output policy modules.

Configure a Release build and run only the five registered acceptance scripts:

```sh
cmake -S . -B build-q2ref -DCMAKE_BUILD_TYPE=Release -DCHIPCAE_BUILD_TESTS=ON
cmake --build build-q2ref -j2
ctest --test-dir build-q2ref -N -R '^vv_q2ref_'
ctest --test-dir build-q2ref -R '^vv_q2ref_' --output-on-failure
```

CTest supplies the source root as argument 1. A script also runs directly from
the source root as `python3 tests/integration/fem_q2_point_ref/test_point_exact.py .`.
Tests never skip and never return 77.

- `point_exact`: 12 boxes with unequal widths in all directions, independently
  shuffled nodes and cells, 500 seeded random points, cell centers, faces, edges
  and vertices; pure bending, every required triquadratic monomial, analytic
  cubic Q2 remainder, temperature, shared faces, frozen mesh and tensor layout.
- `mesh_errors`: all documented error categories, malformed shapes and types,
  validation precedence, geometric and query tolerance boundaries, first
  outside point, missing/empty/broken rank files and bitwise stable gid sorting
  across rank9/rank10 and reversed discovery (including exact int64 ids).
- `same_point_pv`: 12 by 4 strip with translation and rotation, two sampling
  densities, scalar centroid formula bit for bit, analytic plane-removed PV,
  uncovered cells, temperature selfcheck and inclusive selfcheck threshold.
- `richardson_dual`: analytic orders 2 and 3, both signs of error, explicit ratio
  and safety factor, every status, both sides of disagreement, exact binary
  tolerance equality and numerical validation.
- `cli`: two real synthetic rank files, JSON keys/precision, output rejection
  and write errors, dual exit codes 0/1/3, every applicable parser error at all
  levels, numerical errors, stderr first-line prefixes and no exit 77.

Oracles are the polynomial expressions and formulas in the brief. Point fields
use an absolute error bound of `1e-12 * max(abs(analytic_field))`, applied to
each component, including zero components. PV and extrapolated values use
relative error `1e-12`; observed order uses relative error `1e-10`. Bitwise
checks are used only where the brief explicitly requires reproducibility.
Timing is checked only for finite, nonnegative values and has no speed gate.

For symmetric uniform samples, removing a plane from
`a + bx + cy + d*(x-xbar)^2` leaves
`d*((x-xbar)^2 - mean((x-xbar)^2))`. Thus the analytic PV is
`abs(d) * (max((x-xbar)^2) - min((x-xbar)^2))`, with `d = 0.37/2`.

This commit is the RED stage: the target module intentionally does not exist.
An ImportError naming `tools.vv.fem_q2_point_ref` is expected, rather than a skip
or an expected-failure marker. No production stub is needed for Python syntax
checking. The explicit acceptance requirement that omitted `tol` raise
`Q2REF_BAD_ARG` is retained; the brief also specifies a signature without a
default, which requires reconciliation during implementation. Undefined GCI
for a zero fine value is left for a ruling rather than prescribing new behavior.
