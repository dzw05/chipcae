# Q2 thermal stress solver

`chipcae-solve --case case.json --profile profile.json --out run_dir [--cut-output hex]`

Build with JSONcons, yaml-cpp, MPI, HDF5 and PETSc-enabled MFEM. Enable
`CHIPCAE_ENABLE_JSONCONS`, `CHIPCAE_ENABLE_MPI`, `CHIPCAE_ENABLE_HDF5` and
`CHIPCAE_ENABLE_MFEM`. Copper mixtures additionally require the material library
and Kokkos (`Kokkos_DIR`). yaml-cpp 0.8.0 is fetched when unavailable; CMake 4
needs `-DCMAKE_POLICY_VERSION_MINIMUM=3.5`. Bare builds retain input validation
and `--help`, and report an unavailable numerical backend.

JSON/YAML cases resolve references relative to their containing file. Optional
SHA-256 checks precede parsing. The material library container is
`{schema: "chipcae.material_library/1", materials: [material.v1 cards...]}`.
Cards must be constant isotropic thermoelastic materials with single-knot E,
nu and alpha tables; steady conduction also requires positive `conductivity`
in W/(m K). Layer and aligned region materials remain distinct. Copper fractions
call the material module's `CopperMixture` (M7); conductivity is arithmetic in
plane and harmonic through thickness. Polygon copper maps and internal material
cuts are rejected.

A stack outline may be a convex polygon extruded through every layer, with or
without a repeated closing vertex. Clockwise and counterclockwise rings work.
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
systems.

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
and residual-checked iterative refinement. Events include assembly and LU solve
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
estimate. Pure natural manufactured solutions are compared modulo their
constant/rigid nullspaces. The bilayer test compares against an uncut conforming
Q2 wedge mesh of the same polygonal domain; intermediate PV errors may be
non-monotone, while finest error must improve on coarsest and stay below 1%.
A cut-bilayer center-curvature check uses Timoshenko's reference.

The preview digest comparison remains skipped pending its baseline API, and GPU
acceptance remains outside this CPU implementation. Protocol codes remain
MATERIAL_ID_DUPLICATE=11, INPUT_REFERENCE_MISSING=12, CUT_UNSUPPORTED=13 and
INPUT_INVALID=14.
