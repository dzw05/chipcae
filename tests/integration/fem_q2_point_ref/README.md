# FEM Q2 point reference acceptance

All inputs are synthetic analytic fields generated in temporary directories by
`q2_synth.py`. No solver, stored fixture, real case, or optional backend is used.
Python's standard library and NumPy generate the tests; the tool reuses
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
  levels, numerical errors (including finiteness before range checks across
  CLI argument groups), stderr first-line prefixes and no exit 77.

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

The preceding test-only commit recorded all five tests failing with the missing
`tools.vv.fem_q2_point_ref` ImportError. No assertions were weakened in the
implementation stage. A further CLI regression was observed failing before
fixing the order of checks for nonfinite z and finite, invalid bbox/counts.

## Using the reference

`load_fem_q2(run_dir)` reads every `accuracy-q2-rank*.json` in numeric rank order.
Every rank must contain a nonempty `elements` list. It sorts cells by exact
int64 gid and reconstructs all 27 tensor nodes from coordinates, independently
of exporter node order. Only axis-aligned, equally spaced Q2 tensor nodes are
supported. Degenerate or skewed cells and points outside the per-axis cell
tolerance produce `Q2RefError`; there is no extrapolation or solver fallback.

`fem_q2_point_values(mesh, xyz, field="U")` returns displacement vectors, or
temperature scalars for `field="T"`. An AABB tree with tolerance-expanded boxes
locates points; stable traversal resolves shared boundaries. Point batches and
small leaves bound temporary arrays without a points-by-all-cells matrix.
The tree is rebuilt on each public evaluation because the arrays inside the
frozen `Q2Mesh` can still be mutated.

`same_point_pv(mesh, xy, z_top)` first evaluates every cell's nodes through the
public evaluator for both U and T, then evaluates uz at the supplied xy points
and calls the shared plane fitter. Discontinuous shared-node values can fail
this selfcheck. The returned cell count includes all loaded cells.
`eval_seconds` measures selfcheck, index construction, sample evaluation and
plane removal together. Timing has no acceptance threshold.

`uniform_top_centroids` is intended for a uniform, uncut strip with
`padded_cells=0`. Callers with other grids must supply their actual sample xy.

```sh
python3 -m tools.vv.fem_q2_point_ref point-pv --fem-run synthetic-run \
  --bbox 0 0 1 0.2 --nx 12 --ny 4 --z-top 0.1 \
  --data-class synthetic --json synthetic-pv.json
python3 -m tools.vv.fem_q2_point_ref dual \
  --fvm-pv 1 --exact-pv 1.01 --richardson-pv 0.99 --tol 0.02
```

Both commands print full-precision JSON. Errors exit 2 with a `Q2REF_` prefix
on stderr's first line. `dual` exits 0/1/3 for PASS/FAIL/REFERENCE-DISAGREE.
Only MONOTONE Richardson results supply an extrapolate; downstream callers
must stop on any other state rather than substitute another reference.

## Contract tensions and implementation choices

- `dual_reference(fvm_pv, exact_point_pv, richardson_pv, tol)` requires an explicit
  `tol` with no default. Omitting it raises Python's argument-binding `TypeError`.
  Invalid supplied values raise `Q2RefError`; the CLI requires `--tol` and reports
  an omitted option as `Q2REF_BAD_ARG`.
- A MONOTONE Richardson sequence with `f_fine=0` has undefined relative GCI.
  It is rejected with `Q2REF_BAD_ARG` rather than emitting a nonfinite result.
  This conservative choice needs an architect ruling if zero-valued fine
  references are to be supported. Unrepresentable arithmetic also fails
  explicitly; JSON never emits NaN or Infinity.
- The numerical import path uses only standard library and NumPy. The mandated
  `field_compare.ensure_output_allowed` reuse is imported lazily for `--json`;
  that existing module imports h5py. Thus JSON file output retains that existing
  transitive dependency. No dependency stubs, copies or existing-module edits
  are used. Real output is rejected inside any git tree by the shared policy.
- Full-suite CTest and its baseline are left to the pipeline, following the
  final task instruction. Only named acceptance and reused-module tests run
  during implementation. No real FEM calculation is performed.

## Synthetic performance measurement

实测 on Linux with NumPy 2.5.5: 96 by 32 by 2 cells (6144 cells, rather than
the brief's approximate 12000), 3072 top centroids, analytic bending plus rigid
motion. `eval_seconds` was 1.610305022 s; total time including synthetic data
generation and mesh construction was 3.067322371 s. Process peak RSS was
113.83 MiB. Node selfcheck was 2.1985e-16 and PV relative error was 9.1933e-16.
These are observations from one run, not test oracles or performance gates.

Reproduce the timed calculation from the repository root:

```python
import sys
import numpy as np
sys.path.insert(0, "tests/integration/fem_q2_point_ref")
from q2_synth import elements, strip_field
from tools.vv.fem_q2_point_ref import (
    q2_mesh_from_elements, uniform_top_centroids, same_point_pv,
)

data = elements(np.linspace(0, 1, 97), np.linspace(0, 0.2, 33),
                np.linspace(0, 0.1, 3), strip_field)
mesh = q2_mesh_from_elements(data)
del data
xy = uniform_top_centroids(0, 0, 1, 0.2, 96, 32)
print(same_point_pv(mesh, xy, 0.1))
```
