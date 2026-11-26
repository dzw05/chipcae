# Scheme A acceptance tests

These tests prescribe geometry and analytic fields independently of the
`tools.vv.fvm_warpage_gate` implementation. Missing gate code is an assertion
failure. Only missing numpy/h5py exits 77; a skip supplies no acceptance evidence.

The interpolation tests use the internal `(values, interp_indicator)` return
convention. PV means the maximum minus minimum of the residual at the supplied
samples after a least-squares plane fit. The analytic quadratic PV test includes
the midpoint; it does not insert new samples into a centroid set.
The fit centers and scales xy before solving. Interpolation validates a complete
rectangular top lattice, uses deterministic quadratic stencils, and takes the
largest primary/shifted difference across the available x and y directions,
divided by the sampled primary PV. Outside-domain targets, unavailable alternate
stencils and nonpositive reference PV fail as `参考插值不足`.

Gate records have unique `(nx, ny, kz)` keys; duplicates and malformed data raise
`InputInvalid`. Checks are ordered by `(nx, ny, kz, name)`. Missing required
kz4/kz8 runs fail; finite kz16 records remain diagnostic. Limits are inclusive,
without an acceptance epsilon. Preconditions take their legacy limit from the
caller and omit the nodal check only when `nodal_rel` is `None`.
Each judged record also requires finite, nonzero `fvm_curvature` and
`fem_curvature` with the same sign, independently of the error signs. The caller
computes them with the existing `fvm_case_util.curvature` function.

`top_face_centroids(run_dir, requested_nxny)` requires the `(nx, ny)` tuple from
the written case. For an uncut strip it checks that `nx0*2^L`, `ny0*2^L` equal
those requested dimensions, the sample count equals their product, and
`padded_cells` is integer zero. All four manifest fields are required. This
preserves the frozen coarse-grid meaning of `nx0/ny0`. `same_point_error` reads
`case.json` in the result directory (synthetic runs) or beside it (CLI runs);
it fails if the request is missing and never infers it from the manifest.

Crosscheck mesh sequences sort fields together with metadata. Mechanical GCI
uses only `warpage_pv` and rigid-removed `deformation_max`; the existing Celik
implementation also continues to report temperature-rise GCI. Raw `U_mag_max`
retains its values with `convergence_use: false` and the explanation
`U_mag_max 含刚体位移，只打印，不作收敛判据`. Thermal-only quantities remain unavailable.

The revised brief corrects the quadratic sampled centroid/node PV ratio to
`1-2/nx` for even nx and `(nx-1)/(nx+1)` for odd nx. Analytic assertions cover
even nx 24/48/96/192 and odd nx 25 without inserting midpoint samples.
The strip test conservatively retains G1's final-pair in-plane order >=1 alongside
the new thickness order >=1.5; both must pass.

All solver cases are generated in temporary directories from `bimaterial_strip`.
Each kz uses a cached FEM reference, starting at 12x4 and doubling to the first
adjacent pair with relative PV change <=1e-3, choosing the finer member. FEM uses
direct/rtol=1e-10. FVM keeps the existing fixture solver settings. Face PV comes
from the FVM manifest, while the FEM reference is interpolated at face centroids.
Nodal errors remain diagnostic for this strip gate.

Each launch uses a fresh measurement supervisor, monotonic wall time and
RUSAGE_CHILDREN peak RSS. Linux KiB and macOS bytes are normalized to MiB.
Available memory must be >=4 GiB; ordinary runs require RSS <=3072 MiB and
FEM references require wall time <=600 s. Resource failures are failures, never skips. The optional
Darwin/CHIPCAE_VV_HIGHMEM=1 cases retain the brief's RSS cap pending an explicit
high-memory policy; this test does not invent a larger allowance. On Linux,
192x64 and kz16 print their unrun status. Solver/reference failure cannot be
classified as an FVM numerical defect until the gate unit tests pass.

CTest names: `vv_fvm_warpage_gate_unit`, `vv_fvm_warpage_gate_a_strip`.
