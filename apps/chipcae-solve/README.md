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
call the material module's shared M7 law also used by `CopperMixture`; I1 1.2
conductivity is arithmetic in all three directions. Polygon copper maps use
the mesh module’s per-column fractions. Unsupported internal material cuts fail explicitly.

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

## FVM case selection

Select `fvm.conduction` for steady heat and `fvm.solid` for mechanics in
`case.json`. Physics order is immaterial; both entries must use the same
family. The command stays unchanged:

```sh
chipcae-solve --case case.json --profile profile.json --out run-fvm
chipcae-solve --case case.json --profile profile.json --out run-fvm-hex --cut-output hex
```

The adapter solves heat first, then passes CV temperatures minus `T_ref` to
`solve_elastic`. A solid-only case uses `uniform_temperature` minus `T_ref`.
Mechanical stiffness and expansion use the shared `solve::mixture` laminate
helper with the same CV copper fractions as thermal conductivity. Both
libraries receive the case's `rtol`, `max_iter` and `preconditioner` unchanged;
FVM accepts `jacobi` (default) and `gamg`. FEM stabilization and penalty
settings are ignored; shared cut quadrature settings remain validated.

`rigid: "3-2-1"` selects the elastic library's rigid-mode removal. Fixed face
targets map to sides xmin/xmax/ymin/ymax/zmin/zmax. Fixed layer targets expand
to every exterior face of that layer, including all slices of a named layer.
Embedded fixed temperature/displacement constraints remain unsupported.
Thermal fixed temperatures support faces and layers; convection supports
faces, layers and embedded boundaries. Power uses physical target volume.

`warpage_pv` and `U_mag_max` come directly from the elastic result, with methods
`FVM top-face centroids; least-squares plane removal` and `FVM cell centers`.
`T_max` retains the maximum CV temperature and method `FVM cell centers`.
Zero temperature rise still runs mechanics and retains the normal methods.

Point U uses a volume-weighted average of distinct incident CV displacements
in the interior. Exterior vertices use area-weighted incident face values;
prescribed faces take precedence over free faces, including fixed/free edges.
Coordinate keys reconcile copied rings deterministically. Cut-face membership
uses actual planar polygon rings. The same reconstruction supplies regular
hex, cut polyhedron, embedded-boundary and `--cut-output hex` points.
Regular hex T remains the thermal library's `T_vertex`; new cut/EB T retains
the existing incident-CV first-order reconstruction and Dirichlet precedence.
Topology, parent GIDs, physical volumes and copper diagnostics are retained.

A successful thermal solve emits one rank-0 level-1 `FVM_ENERGY ` JSON record.
A successful elastic solve emits one rank-0 level-1 `FVM_ELASTIC ` JSON record
with exactly `iterations`, `residual`, `warpage_pv` and `U_mag_max`, using the
classic locale and 17 significant digits. Final residual points identify
`fvm.conduction`/`T` and `fvm.solid`/`U` respectively. STOP is checked before
mechanics and immediately after its collective call, before failure handling
or successful mechanics/output publication. A STOP queued during a solve
therefore takes effect once the solve returns.

The frozen I14 1.0 library supports cut CVs and embedded faces. The adapter
preserves CV order and sets `is_cut[c] = (cv_kind[c] != 0)`, including merged
CVs. Exterior grid side 6 becomes side -1 with `is_embedded=1`; `z_top` is the
upper grid z coordinate. Result validation checks cell/face arrays, the number
of top samples (including qualifying embedded top faces), finite values and
the library diagnostics before publication.

Named error prefixes keep their existing codes. Both `INTERNAL:` and the
known I14 `INPUT_INVALID: INTERNAL:` producer prefix map to INTERNAL (1).
Unclassified mechanical solver failures also exit 1, matching FEM; invalid
metadata exits 14. The legacy embedded-face unsupported diagnostic uses
ordinary INPUT_INVALID handling (exit 14); supported cut geometry follows the
normal solve path.

On failure, the CLI preserves the library's opaque reason text, exits 1 for
numerical failures and publishes no successful mechanics result. Frozen I14
exposes no structured failure residual. The November 21 18:02 ruling removes
that requirement; successful `fvm.solid` residual publication remains required.

Acceptance compares CLI PV and maximum displacement with an independent
public-I14 replay for the imported synthetic board, uniform bilayer,
layer-prescribed-temperature bilayer and cut bilayer. All use the I14 relative
tolerance of 1e-8 and retain independent material, temperature, geometry and
support oracles. FEM curvature signs and np=1/2 agreement remain gates.
The complete original FEM PV comparison with its 0.10 bound is retained for
`solve-elastic-accuracy`, outside this card's gate, under
`handoff/rulings/fvm-wire-elastic.md`. Demo failures must preserve the original
library reason; convergence is not required by that ruling.

The board vertex-temperature rise retains upstream's temporary 0.02 gate and
restoration comment; the demo retains its 0.01 gate. `FEM_SNAPSHOTS` and `probe`
match merged main b1cf5fa verbatim. The independent stiffness, CTE and conductivity
oracles follow I1 1.2 with their original tolerances. CLI/direct-library
equivalence is this card's numerical gate; FEM PV and displacement differences
remain accuracy diagnostics for `solve-elastic-accuracy`.
See [FVM acceptance](../../tests/integration/fvm_case_e2e/README.md) for measured
values and remaining merge gates. Build and test through the shared wrappers,
retaining their PATH precedence:

```sh
cmake --build "$build" -j2
ctest --test-dir "$build" -j1 -R '^fvm_fvm_case_e2e$' --output-on-failure
```
