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
the factor; no inverse is used for recovery. Assembly gives
`S lambda = b source/alpha`, plus positive Robin diagonals. A fixed CV
contributes its uncondensed operator and prescribed cell value. Recovery is
`u = (source + b^T lambda)/alpha`, `q = M^-1(u 1-lambda)`.
Local dense arithmetic uses extended precision to limit cancellation.

`ThermalProblem::k` supplies final positive diagonal conductivities, copied
unchanged to `k_used`. Callers use `material::copper_laminate_conductivity`
with `copper_fraction(grid, mesh)` to construct mixtures. Layer IDs are the
one-based values in `attr_table.layers`; copper arrays use layer minus one
and exclusively `elem_gid/nz` for the Morton column. Hex coordinates determine
geometric indices, independently of gids and element ordering.

Targets are `face:xmin` through `face:zmax`, `layer:<name>` for the layer's
exterior faces, and `boundary:embedded`. The optional `ThermalBc::face`
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
The manual `chipcae_fvm_thermal_stress --output <external-directory>` scan is
outside CTest and exits nonzero if any numerical gate fails; its raw JSON
must stay outside the source tree. Extreme thinness and contrast remain
measured numerical risks, with no relaxation of acceptance tolerances.

References: K. Lipnikov, G. Manzini and M. Shashkov,
[Mimetic finite difference method, JCP 257 (2014), 1163–1227](https://doi.org/10.1016/j.jcp.2013.07.031);
K. Lipnikov and G. Manzini,
[Discretization of mixed formulations of elliptic problems on polyhedral meshes](https://arxiv.org/abs/1610.05850).
These are method references; this implementation does not copy prototype code.
