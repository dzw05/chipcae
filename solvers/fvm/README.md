# FVM thermal library

The optional steady thermal solver uses cell averages and planar face averages
on Cartesian control volumes. The conjugate-heat time-step placeholder remains
available independently. Thermal sources and tests require both
`CHIPCAE_ENABLE_MFEM` and `MFEM_USE_PETSC`; PETSc is supplied by the `mfem`
target, without calling MFEM finite-element APIs.

For each CV, `N` contains outward area vectors and `R` contains face-centroid
minus volume-centroid rows. Geometry must satisfy
`maxabs(N^T R - V I)/V <= 1e-10`. The common local implementation constructs
`M0 = R K^-1 R^T/V`, obtains `P = I - Q Q^T` with reorthogonalized thin QR,
and uses `M = M0 + P Gamma P` with the specified diagonal stabilization and
`1e-8` gamma floor. The optional scalar comparison uses
`gamma = trace(M0)/3`. Since `R^T N = V I` and `P N = 0`,
`M N K = R`; affine fluxes are consistent on concave CVs as well as boxes.
There is no centre-distance clipping, convexification, or eigenvalue clipping.
Symmetric eigenvalues give the reported local condition number.

On a box, each axial pair has
`M0 = a [[1,-1],[-1,1]]`, `P = 0.5 [[1,1],[1,1]]`, `gamma = 2a`.
When the floor is inactive the resulting `M` is diagonal, with
`Mff = h/(2 k A)`. Eliminating the shared face produces the distance-weighted
harmonic TPFA conductivity. Boxes use the same MFD implementation.

Cholesky solves give `b = M^-1 1`, `alpha = 1^T b` and columns of
`S = M^-1 - b b^T/alpha`. Only this condensed operator is stored, alongside
the factor; no full inverse is stored. Assembly gives
`S lambda = b source/alpha`, plus positive Robin diagonals. A fixed CV
contributes its uncondensed operator and prescribed cell value. Recovery is
`u = (source + b^T lambda)/alpha`. Flux recovery uses
`q = b source/alpha - S lambda`, with the identical row-sum-adjusted `S`
used in assembly. For a fixed CV it replaces `source` by
`alpha u_fixed - b^T lambda`. Evaluating row differences preserves the
constant nullspace without subtracting large temperature origins.
Local dense arithmetic uses extended precision to limit cancellation.

`ThermalProblem::k` supplies final positive diagonal conductivities, copied
unchanged to `k_used`. Callers use `material::copper_laminate_conductivity`
with `copper_fraction(grid, mesh)` to construct mixtures. Layer IDs are the
one-based values in `attr_table.layers`; copper arrays use layer minus one
and exclusively `elem_gid/nz` for the Morton column. Hex coordinates determine
geometric indices, independently of gids and element ordering.

Targets are `face:xmin` through `face:zmax`, `layer:<name>` for the layer's
exterior axial faces, and `boundary:embedded`. Layer targets exclude embedded
faces, including when an explicit face selector is supplied. The optional `ThermalBc::face`
selects one public face within its target, permitting spatially varying MMS
averages. Duplicate prescriptions fail. Flux density is positive inward;
returned integrated flux is positive outward from the face's `lo` CV.
Unspecified boundaries are insulated. Every connected component needs a
Dirichlet face, positive Robin coefficient, or fixed CV.

The solve uses `T-T_ref`. The default origin is the area-weighted mean of
Dirichlet and positive Robin temperatures; fixed-only problems use a
volume-weighted fixed temperature. Face unknowns are numbered contiguously
by the owner rank of their `lo` CV. Only owned CVs contribute to `MATMPIAIJ`;
PETSc sums off-rank rows. CG uses Jacobi or GAMG and checks convergence.
The globally replicated input convention lets every rank recover identical
cell/face/vertex fields and diagnostics after gathering face values.

Conservation diagnostics recover both sides independently. Cell balance
excludes prescribed CVs and divides by `max(sum(abs(q)),abs(source),1e-300)`.
Face jumps divide by `max(abs(qlo),abs(qhi),1e-300*max(abs(q)))`.
The signed energy ledger contains cell sources, boundary inflows `-q`, and
fixed reactions `sum(q)-source`; heat in/out are positive/negative ledger
parts and imbalance divides by their sum. Diagnostics are physical flux
checks, independent of the KSP residual.

Vertex values retain input hex ordering: interior values use incident CV
volume weights; boundary values use incident face area weights, with
Dirichlet faces taking precedence. Conflicting Dirichlet corner values are
area averaged. Vertices without retained incidence take `T_ref`.

Build using the site's configured MFEM package:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCHIPCAE_BUILD_TESTS=ON \
  -DCHIPCAE_ENABLE_MFEM=ON -DMFEM_DIR="$MFEM_DIR" -DCHIPCAE_ENABLE_MPI=ON
cmake --build build --parallel 2
ctest --test-dir build -R '^solve_fvm_thermal$' --output-on-failure
ctest --test-dir build -R '^solve_fvm_thermal_np2$' --output-on-failure
ctest --test-dir build -R '^solve_fvm_thermal_cut$' --output-on-failure
```

On shared build hosts keep the build-slot wrapper first in `PATH`.
Only the thermal acceptance executables are CMake targets, and both run under
CTest, with an additional two-rank run when MPI is enabled. The manual diagnostic
source `tests/test_fvm_thermal_stress.cpp` is retained for external builds, outside
the project's targets and CTest suite. That scan takes
`--output <external-directory>` and exits nonzero if any numerical gate fails;
its raw JSON must stay outside the source tree. Extreme thinness and contrast
remain measured numerical risks, with no relaxation of acceptance tolerances.

References: K. Lipnikov, G. Manzini and M. Shashkov,
[Mimetic finite difference method, JCP 257 (2014), 1163–1227](https://doi.org/10.1016/j.jcp.2013.07.031);
K. Lipnikov and G. Manzini,
[Discretization of mixed formulations of elliptic problems on polyhedral meshes](https://arxiv.org/abs/1610.05850).
These are method references; this implementation does not copy prototype code.

Cut geometry uses existing `CutLayerArrays` only. Current producers segment
volume and surface quadrature by cut parent, with exactly one subcell per
record. Multiple subcells per record return `NOT_IMPLEMENTED`, since those
segments do not establish quadrature ownership. Reference volume weights
are multiplied by the physical parent Jacobian; surface weights already
contain physical area. Positive-volume unlisted slivers are retained.
Embedded quadrature is assigned to independently oriented triangle planes
at tolerance `1e-12 * min(parent edge)`. Axial openings must agree in area
and centroid before sharing a face. Embedded faces support Robin or default
insulation. They bound the retained solid; shared interfaces between adjacent
Cartesian parents are axial openings. Internal embedded material interfaces
are outside the cut-geometry contract.

Only explicitly listed `small_subs` merge, choosing maximum total shared
opening area among neighbors of the same material and layer. Equal areas
use the smallest `(parent gid, subcell ordinal)` identity. The representative
is the smallest constituent identity and supplies the merged owner rank.
`constituent_off/elem/gid/subcell` preserve every original constituent;
`cv_subcell` is the stable parent-local ordinal. Internal faces disappear,
while all remaining pieces retain their actual centroids and outward areas;
no convex hull replaces the union. Volume, centroid and pre-merge copper
fraction are volume weighted, and the shared geometry gate runs again.
`FvFace::parent_elem` retains the original piece for vertex reconstruction.
Cross-material/layer merges with no eligible neighbor return
`NOT_IMPLEMENTED`; the public thermal problem has no constituent material
pair from which to reconstruct a cross-material conductivity. Callers build
merged conductivities using the returned merged copper fraction and the
existing material mixing helper.

Hierarchy padding in the current background producer adds subdivisions
inside the physical domain. Those retained physical hexes remain CVs;
removing them would remove material. Omitted void elements and explicit
`void_elem` records generate no CV. Outer `face:*` targets require the
actual outer coordinate plane, so an internal void boundary cannot anchor
a disconnected component accidentally.

The public solver accepts the existing `chipcae::Communicator` world
snapshot, constructed after runtime initialization. MPI headers and native
communicator handles remain in the private backend; serial-reference tests
use that backend on the self communicator. A stale world snapshot is rejected.
`Communicator` stores only world rank and size and cannot wrap a subcommunicator.
Error codes use existing names as prefixes in the unchanged
`mesh::Error::reason` carrier rather than adding a frozen schema error type.

Precisely, the diagonal stabilization is
`gamma_f = max(2 r_f^T K^-1 r_f/V, 1e-8 max_g(2 r_g^T K^-1 r_g/V))`.
Floor activations are counted per face of each CV; the reported condition
maximum and its representative gid include fixed CVs as well.

Public CV order is parent-gid order before merging and representative-gid
order afterward; ordinals are parent local and start at zero. Constituent
lists keep this stable order. `cv_volfrac` for a merged CV is union volume
divided by the sum of its constituent parent volumes. `copper_fraction`
returns the grid's stored precomputed fractions, including volume-weighted
merged values; it does not re-read a different mesh's copper map.
An entirely zero-flux solve defines its zero-over-zero face jump as zero;
any nonzero flux uses the exact denominator above, without a practical
absolute floor or flux clipping.

For the particular quadratic `u=x^2+y^2+z^2` on uniform cubes of width `h`,
this diagonal box operator has exact face averages and integrated fluxes,
with cell average `u_exact_average - h^2/3`. Indeed,
`lambda_f = |C_f|^2 + h^2/6` and
`u_c = |C_c|^2 - h^2/12` give
`2 k_i A_f/h * (u_c-lambda_f) = -k_i S_f,i * 2 C_f,i`.
Thus the temperature error is second order but a flux observed-order test
on this fixture compares rounding errors rather than a nonzero truncation
error. A dedicated uniform-cube regression checks those exact face averages,
fluxes, and the cell shift. The quadratic order tests use smoothly graded
Cartesian meshes with nonzero truncation error and the same order thresholds.
Layered tests cover insulated single columns and multiple columns with a
known transverse affine field, so the exact relative interior-jump check
measures nonzero flux. Explicit zero-flux patch and insulation checks remain.
The retained-sliver fixture supplies analytic sub-box quadrature at thickness
`2e-10`, retaining all sixteen positive slivers and the strict opening gates;
this avoids generic clipping's centroid cancellation at that aspect ratio.
An explicit face selector may address any exterior piece with the target's
side orientation, including an exposed interior void boundary after merging;
a target-wide `face:*` prescription selects the global outer coordinate plane.
