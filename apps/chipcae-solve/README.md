# Thermal and thermal stress solver

`chipcae-solve --case case.json --profile profile.json --out run_dir [--cut-output hex]`

Build with JSONcons, yaml-cpp, MPI, HDF5 and PETSc-enabled MFEM. Enable
`CHIPCAE_ENABLE_JSONCONS`, `CHIPCAE_ENABLE_MPI`, `CHIPCAE_ENABLE_HDF5` and
`CHIPCAE_ENABLE_MFEM`. Copper mixtures use the material library's shared laminate helper;
material-point comparison tests additionally use Kokkos (`Kokkos_DIR`). yaml-cpp 0.8.0 is fetched when unavailable; CMake 4
needs `-DCMAKE_POLICY_VERSION_MINIMUM=3.5`. Bare builds retain input validation
and `--help`, and report an unavailable numerical backend.

JSON/YAML cases resolve references relative to their containing file. Optional
SHA-256 checks precede parsing. The material library container is
`{schema: "chipcae.material_library/1", materials: [material.v1 cards...]}`.
Cards must be constant isotropic thermoelastic materials with single-knot E,
nu and alpha tables; steady conduction also requires positive `conductivity`
in W/(m K). Layer and aligned region materials remain distinct. Copper fractions
call the material module's shared M7 law also used by `CopperMixture`; conductivity is arithmetic in
plane and harmonic through thickness. Polygon copper maps use the mesh module’s
per-column fractions. Unsupported internal material cuts fail explicitly.

A stack outline may be a convex polygon extruded through every layer, with or
without a repeated closing vertex. Vertices must lie inside `outline.box`.
When the inline polygon is absent, a hash-checked `external_refs` polyset may
supply one ring in its `outline` layer. Clockwise and counterclockwise rings work.
The solver constructs the complete bounding grid, clips it with mesh-owned plane
half-spaces, removes exterior cells and their exclusive DOFs, and preserves
surviving element/node GIDs. Layer interfaces remain aligned with z faces.
Non-convex outlines, holes, general curved solids and internal material cuts
are unsupported. A sphere in numerical tests is an explicitly faceted convex
plane approximation, refined together with the background mesh.

For example, `mesh.cut` accepts:

```json
{"stabilization":"ghost_penalty", "quad_degree":6, "gamma_g":0.1}
```

Quadrature degrees 2, 4, 6 and 8 are supported; the default is 6. All cut volume,
embedded and clipped Cartesian face integration rules come from the mesh module.
FEM checks positive integrated volume and agreement with polyhedron volume to
relative tolerance 1e-12. Ghost stabilization includes first and second normal
derivative jumps with `h_perp` and `h_perp^3`, scaled by thermal conductivity or
normal elastic stiffness (`2 mu + lambda` for isotropic material). Setting
`gamma_g` to zero disables stabilization and can produce singular small-cut
systems. Across thermal material interfaces the jumps are conductivity weighted;
across elastic material interfaces curvature is extended without penalizing the
physical piecewise affine strain jump.

Heat is solved before elasticity, with T evaluated in the same H1 Q2 space.
Embedded boundaries support natural insulation, traction-free mechanics and
convection. Embedded fixed temperature/displacement fail explicitly with
CUT_UNSUPPORTED: Nitsche is not implemented. Cartesian fixed temperatures,
convection, volumetric heat sources and fixed face/layer displacements retain
the O2 behavior. Total power is divided by the actual cut target-layer volume;
a zero-volume target fails. Rigid 3-2-1 constraints use three non-collinear
vertices of fully interior regular cells and validate exactly six constrained
DOFs. A domain without suitable regular cells fails explicitly.

The FP64 CPU backend uses PETSc sequential sparse LU with exact row preallocation
and residual-checked iterative refinement. Symmetric diagonal basis normalization
is applied before factorization; residual acceptance uses the original system. Events include assembly and LU solve
times. Every MPI rank solves the complete global problem on PETSC_COMM_SELF;
np agreement verifies reproducibility and output ownership, **not distributed
solver scalability**. Memory grows with rank count. Each serial interior ghost
face contributes once to each independent global solve; mesh ownership metadata
is preserved for a future distributed backend. `column_gmg` warns and uses LU.

Results use the existing VTKHDF 2.8 writer. Blocks, in order, are `hex`, `cut`,
`eb_surface`, `particles`. Regular cells are hexes; cut cells are polyhedra with
per-cell outward face rings. Mesh-owned triangles define the EB surface,
independently of quadrature degree. T/U on cut and EB vertices use parent-cell
Q2 interpolation. `--cut-output hex` writes active background hexes with
`cell_kind=1` for cuts and keeps EB triangles. This changes output only.

Warpage PV uses top-domain Q2 nodes and mesh cut-top quadrature samples after
least-squares plane removal; the summary's `method` records this choice.
`T_max`, `U_mag_max` and the global mesh digest are also reported. The digest
uses the surviving background mesh, ordered by GID, with the same O2 byte
contract. Selected fields, CAELOG1 events and STOP handling remain supported.

Run the packaged two-material cut-corner example:

```sh
chipcae-solve --case examples/s2_solve/case.json \
  --profile examples/s2_solve/profile.json --out run-s2
```

CTest keeps the O2 pipeline checks, adds cut CLI/VTKHDFReader and np=1/2 checks,
manufactured heat/elasticity convergence at four mesh sizes and degrees 4/6/8,
geometry closure/divergence-volume checks, and 50-position thermal/mechanical
spectral scans at aspect ratios 1 and 1:10. The small spectral tests use dense
symmetric eigenvalues, both unscaled and Jacobi-scaled, instead of a Lanczos
estimate. The tenfold variation bound applies to the Jacobi-scaled system used
by the direct solver; raw estimates are reported separately. Pure natural manufactured solutions are compared modulo their
constant/rigid nullspaces. The bilayer test compares against an uncut conforming
Q2 wedge mesh of the same polygonal domain; intermediate PV errors may be
non-monotone, while finest error must improve on coarsest and stay below 1%.
A cut-bilayer center-curvature check uses Timoshenko's reference.

The solver/preview mesh digest comparison runs at np=1/2 after the O2 merge. GPU
acceptance remains outside this CPU implementation. Protocol codes remain
MATERIAL_ID_DUPLICATE=11, INPUT_REFERENCE_MISSING=12, CUT_UNSUPPORTED=13 and
INPUT_INVALID=14.

Run profiles accept case-sensitive `backend=cpu` with `gpus=0`, or
`backend=cuda` with `gpus>=1`. A CUDA profile selects `mfem::Device("cuda")`
only when built with CUDA-enabled MFEM, and reports the selected device.
Otherwise it exits with NOT_IMPLEMENTED (2) and
`chipcae-solve built without CUDA-enabled MFEM`. Invalid backend/GPU-count
pairings exit with INPUT_INVALID (14). Device selection keeps the existing
solve path; it does not promise GPU execution or acceleration.

The CUDA-enabled MFEM branch has a compile-only acceptance requirement; this
requirement has **not been verified (未实测)**. The branch has not been compiled
or runtime-tested on this box, and current CI does not compile it. No successful
CUDA-enabled MFEM compilation or GPU execution is claimed.
`cuda-kokkos-core-compile` covers Kokkos and core only; a `cuda-solve-compile`
job is outside this task. Selecting the global MFEM device
may expose host/device memory incompatibilities in the existing PETSc solve path.

The amended X1 brief permits the additive public `RunInputs::backend` string
member (default `"cpu"`) in `include/chipcae/solve/case_loader.hpp`. This changes
the struct layout/ABI and requires consumers to rebuild; function signatures
and solver/kernel interfaces are unchanged.

## FVM case selection (thermal-only stage)

Select `fvm.conduction` for steady thermal physics and `fvm.solid` for the
mechanical physics entry in `case.json`; physics order is immaterial. Both
entries must belong to the same family. The command stays unchanged:

```sh
chipcae-solve --case case.json --profile profile.json --out run-fvm
chipcae-solve --case case.json --profile profile.json --out run-fvm-hex --cut-output hex
```

FVM accepts `solver.linear.preconditioner` values `jacobi` (also the default
when `solver.linear` is absent) and `gamg`. It copies `rtol` and `max_iter` to
the thermal library. FEM stabilization and penalty settings are ignored for
FVM; shared cut quadrature settings are still validated. Fixed temperatures
support faces and layers; convection supports faces, layers and embedded
boundaries. Layer convection covers every exterior face of the selected layers,
including embedded faces and all slices of a split-layer alias. Power uses
actual physical target volume, including cut cells; `q_vol` supplies density
directly through the same `heat_source_density` conversion used by FEM.
Embedded fixed temperature/displacement constraints remain unsupported.

This stage solves heat only. All output U values and both mechanical summaries
are zero, with method and level-2 warning `not solved: fvm thermal-only stage`.
These zeros are placeholders, not warpage predictions. Linking an available
elastic library does not activate mechanics. A solid-only case with valid
`uniform_temperature` writes that temperature and zero U.

`T_max` uses the maximum CV temperature and method `FVM cell centers`.
Regular hex vertices use the thermal library's `T_vertex`. New cut/EB points
use volume-weighted incident CV first-order reconstructions from `T_cell` and
`T_face`; incident Dirichlet faces take precedence with area weighting at
conflicting corners. Mesh-owned topology, parent GIDs and physical cut volumes
are retained. `--cut-output hex` retains active cut parents and EB triangles.
Copper fraction and conductivity diagonals are populated when supported by the
results interface. Mechanics C/alpha mixture wiring is deferred.

Each successful thermal solve emits one rank-0 level-1 `FVM_ENERGY ` record,
containing exactly the library's `heat_in`, `heat_out`, `imbalance`, and
`iterations`. The final residual batch uses `fvm.conduction`. STOP is checked
after the solve and before step publication; nonconvergence publishes no step.

Acceptance and measured results are recorded in
[the FVM integration README](../../tests/integration/fvm_case_e2e/README.md).
FVM always writes physical `CellData/volume` for both hex and cut cells,
including when `output.fields` omits it. An explicit `volume` selection is
accepted only for FVM; FEM retains `invalid output field`. The default fields
list is unchanged. Hex volumes are optional writer input: callers that supply
none retain the existing output without hex volume. Cut output and unit
metadata are unchanged.

The board 1% gate independently takes the maximum of each result's hex
`PointData/T`, then compares temperature rises above 298.15 K. Both summary
maxima are printed separately; the FVM summary retains its cell-center
definition. Energy imbalance must remain at most 1e-9; the board and DEMO
embedded-Robin checks explicitly solve with `rtol=1e-12`.

Run the FVM CLI acceptance with the shared build wrappers:

```sh
cmake --build "$build" -j2
ctest --test-dir "$build" -R '^fvm_fvm_case_e2e$' --output-on-failure
```

Local Release measurements (2026-11-20, 实测), including CLI startup; RSS
uses wait4 maximum child RSS and is not a sum across MPI ranks:

| Case/backend | Wall seconds | Peak RSS KiB |
| --- | ---: | ---: |
| imported board, FVM np=1 | 1.533149 | 50428 |
| same board, FEM/direct | 1.790768 | 61804 |
| DEMO-GATE, FVM | 1.569853 | 55764 |
| original DEMO-GATE, FEM | 39.224233 | 1311676 |

The board vertex maxima are FVM 323.13920209610802 K and FEM
323.17794030362211 K: temperature-rise difference 0.1547798462%, below 1%.
Board energy imbalance is 1.3544070916991032e-12 (np=1) and
4.4733543527735244e-14 (np=2). DEMO embedded Robin returns imbalance
1.2088229951858545e-13 in 445 iterations at explicit `rtol=1e-12`.
The largest np=1/2 temperature relative difference is 1.1015562826700222e-14.

Fresh local CTest: `fvm_fvm_case_e2e` passes all 19 methods (97.27 s),
`k1_results_validation*` passes 2/2 (4.24 s), and the existing VTKHDF
regressions pass 5/5 (56.69 s), with no skips or threshold changes.
