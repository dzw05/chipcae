# Cell-centered thermoelastic finite volumes

The library accepts replicated control volumes, oriented faces, engineering stiffness
matrices and thermal strains. It has no CLI or temperature-solver dependency. The caller
owns MPI/PETSc initialization and constructs materials with the existing
`material::copper_laminate` compliance adapter. All public quantities use SI units.

`ElasticMesh::gid` is a unique external key. Sparse, descending or interleaved gids are
valid. Internal permutations sort by owner rank and gid; results keep caller cell/face
order on every rank. Identical prescribed face values merge; conflicting values fail.
Every Fixed side/layer target must match a nonembedded exterior face. Embedded faces
remain free. Errors reuse `mesh::Expected` and `mesh::Error`; invalid input reasons start
with `INPUT_INVALID`. No new error code is defined.

## Discretization

Each cell gradient is an affine operator in neighboring cell unknowns. Coordinate-scaled
weighted least squares includes neighbor differences, prescribed face displacements and
traction-free constitutive closure. A rank-revealing one-sided Jacobi SVD avoids normal
equations. The same boundary closure reconstructs free face displacements, including a
single cell or a column of centers. Engineering shear order is yz,xz,xy and the normal
thermal strains are alpha_x*dT,alpha_y*dT,alpha_z*dT.

The final discretization uses an energy-conjugate finite-volume gradient:
`V G = sum_f (U_f-U_cell) tensor n A`. Internal face values interpolate cells with
an implicit tangential centroid correction; exterior values are prescribed or
boundary-consistent least-squares extrapolations. Cell strain energy uses the caller's
full stiffness and thermal eigenstrain. A face energy penalizes the compact-gradient
residual `deltaU-Gbar*d` using the harmonic acoustic tensor and `area/(n.d)`. This
is the implicit over-relaxed normal correction; it vanishes for affine fields and
interior quadratic bending fields. Actual centroids, normals and areas are used on
both tensor grids and clipped polyhedra.

Differentiating these energies gives symmetric block-coupled equations. Differences
are routed through deterministic shortest paths in the local face graph to retain
one oriented integrated traction per physical face, inserted with opposite signs in
its adjacent cells. This enforces force and moment conservation together. Prescribed
boundary reactions follow the same energy derivative; free face tractions are zero.
The complete implicit stencil is preallocated, including beyond second-ring columns
introduced by the adjoint and nonorthogonal reconstructions.

This energy-conjugate assembly is a substantive deviation from the planned direct
compact-traction stencil. The direct stencil passed affine patches but failed the
left rotational nullspace; local force-couple projections introduced bending stiffness
errors. The final variational form satisfies both rigid nullspaces without such a
projection. It uses volume-weighted cell thermal stress in its consistency term and
harmonic face stiffness in its compact stabilization, rather than a separately
interpolated face thermal-stress load.

For componentwise harmonic stiffness, denominator weights are the respective
cell-to-face distances. A structural zero stays zero; same-sign couplings retain their
sign, and opposite-sign couplings use bounded arithmetic interpolation. The complete
face tensor must still pass scaled Cholesky. These choices resolve coefficient behavior
left open in the implementation plan.

## Linear solve and reconstruction

PETSc assembles only owned AIJ rows with block size three and complete stencil
preallocation. The symmetric matrix uses CG with Jacobi; GAMG uses flexible GMRES(50) because
the multigrid cycle need not be positive definite on singular coarse spaces. The flexible
variant retains actual preconditioned vectors, avoiding restart residual breakdown when
the two-level correction is numerically nonlinear. This is a recorded change from the
planned ordinary GMRES(50). A small
algebraic Ritz space of quadratic cell polynomials, grouped into at most eight layer
bands and enriched with bounded longitudinal Legendre modes for long strips, accelerates the selected Jacobi or GAMG base preconditioner through a balanced
two-level correction through a PETSc shell around the requested base PC. It uses the assembled matrix, not beam formulas or analytic
reference fields. This addition addresses slowly converging bending modes. GAMG's
serial coarse blocks use SVD pseudoinverses, so rigid modes do not lead to unstable LU
pivots. The Krylov operator remains the physical finite-volume matrix.

The relative target is `rtol*||b||2`. On ill-conditioned slender bodies a double-valued
displacement can have a larger unavoidable force residual. The explicit stopping floor
is proportional to machine epsilon times `||A||inf*||U||2`; the final physical residual
must satisfy the larger of the requested target and `8*epsilon*||A||inf*||U||2`.
Nonconvergence is an error containing the reason, iterations and recomputed physical
residual. The one-iteration acceptance case remains a failure. This floating-point
backward-error criterion is another recorded deviation from a strict relative-only
interpretation of rtol.

Pure-traction bodies attach exact rigid nullspaces and verify transpose compatibility
before projecting the load. Disconnected bodies get independent modes. Collinear or
coincident centers use only independent modes. Returned displacements have zero
arithmetic translation and rotation on each body; no cell is pinned. Uniform thermal
eigenstrain supplies an affine warm start whose convergence is checked against the
assembled physical equations. GAMG receives centroid coordinates and rigid near-nullspace
data. The normalized matrix near-nullspace is used directly: a raw `PCSetCoordinates`
call would override it with coordinate-scaled modes. Empty owner ranks participate in
all collectives and receive the same near-nullspace mode count.

Solutions are gathered once, restored to caller order and reconstructed identically on
every rank. Prescribed face outputs are exact. Internal faces interpolate reconstructions
from both sides. `U_mag_max` uses cell centers. Top samples use side 5 or upward embedded
faces at z_top; a centered, scaled rank-revealing plane fit removes tilt. Empty, collinear
and single-sample sets are supported. Method strings are:

- `FVM cell centers`
- `FVM top-face centroids; least-squares plane removal`

## Reproduction

This subdirectory requires `CHIPCAE_ENABLE_MFEM AND MFEM_USE_PETSC`. Test executables
additionally require `CHIPCAE_BUILD_TESTS`. The parent `solvers/fvm/CMakeLists.txt` must
add this directory; the thermal/chief integration owns that hook. Local verification
used a temporary parent hook, removed before commit.

```sh
cmake -S . -B build-fvm-elastic -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCHIPCAE_ENABLE_MFEM=ON -DCHIPCAE_ENABLE_MPI=ON \
  -DMFEM_DIR="$MFEM_DIR" -DCHIPCAE_BUILD_TESTS=ON
cmake --build build-fvm-elastic --target chipcae_test_fvm_elastic chipcae_test_fvm_bimetal -j2
ctest --test-dir build-fvm-elastic -R '^solve_fvm_' --output-on-failure
cmake -S . -B build-fvm-elastic -DCHIPCAE_FVM_ELASTIC_BENCHMARK=ON
ctest --test-dir build-fvm-elastic -R solve_fvm_elastic_benchmark -V
```

Tests use only synthetic tensor and analytically clipped meshes. Timoshenko tests use
nu=0 plies to isolate the beam reference, three factor-two refinements, four cells per
ply initially and central-half curvature fits. The orthotropic CLT test imposes
plane-strain kinematics on lateral y faces; it does not infer plane strain from free
finite-strip boundaries. Assertions remain active in Release builds.

## References and limits

Method references from the brief: Demirdzic & Muzaferija, CMAME 125 (1995), 235–255;
Jasak & Weller, IJNME 48 (2000), 267–287; Cardiff, Tukovic, Jasak & Ivankovic,
IJNME 108 (2016), 1572–1592. Nonorthogonal over-relaxation follows Jasak's 1996
thesis, section 3.3. PETSc documents right/transpose nullspace compatibility under
[MatSetNullSpace](https://petsc.org/release/manualpages/Mat/MatSetNullSpace/).

Small cut volumes must already have been merged by the caller. This is linear,
small-strain elasticity with axes-aligned thermal expansion tensors; no contact,
plasticity, nonlinear geometry, vertex reconstruction or FEM comparison is included.
The full replicated mesh and implicit second-ring gradient operators consume memory
on each rank. Componentwise harmonic interpolation can reject an SPD pair if its
interpolated face tensor loses positive definiteness.

## Measured Release verification

**实测**, optimized Release with runtime acceptance checks, PETSc 3.26.1:

| Acceptance | Result |
| --- | --- |
| Elastic patches, errors, supports, reconstruction and clipping | 110 cases pass |
| MPI np=2, both supports/PCs, zero-owned-row ranks | 13 cases pass |
| Signed Timoshenko heating, 512/4096/32768 cells | errors 2.2741%, 0.5590%, 0.1392% |
| Timoshenko observed orders | 2.0243, 2.0060; cooling gives the same convergence |
| Orthotropic f=0.3 plane-strain CLT | signed curvature 0.362605596231; relative error 2.16e-12 |
| Matrix symmetry defect | `||A-A^T||F/||A||F = 4.69e-17` |

The **实测** 68x68x6 benchmark has 27,744 cells and 83,232 displacement unknowns:

| Base PC / Krylov method | solve_elastic wall (s) | Iterations | Independent `||A U-b||2` (N) | Reported residual (N) | Process peak RSS (KiB) |
| --- | ---: | ---: | ---: | ---: | ---: |
| Jacobi / CG | 38.2305 | 3446 | 2.69502e-8 | 2.69424e-8 | 399836 |
| GAMG / flexible GMRES(50) | 28.5432 | 695 | 1.60256e-8 | 1.60268e-8 | 576976 |

The RHS norm is 7572.82 N. Wall times include validation, assembly, the coarse space,
KSP and reconstruction on a shared build box. RSS uses cumulative process `ru_maxrss`
across both cases and the intervening residual diagnostics. Physical residuals use the
original, unprojected load. The documented backward-error floor was active.

No acceptance assertion was loosened or removed. The public signature and result method
strings are preserved. The committed delivery order is acceptance tests, tensor-grid
solver, then embedded-face activation and documentation. A temporary validation gate
kept embedded-face cases RED in the tensor-grid commit; the geometric operators are
shared by both paths.

The broader suite has two unresolved exceptions outside this module's file scope.
`stack_writer_outline_polyset` fails with `duplicate material id`: its assembly test
helper appends a `copper` card although the example material library already contains
that id. The fine solve in `solve_column_gmg_fcbga_scaling` was terminated by signal 9
in both the initial run and recheck. All four FVM entries pass. The final external
verification log records the full-suite results, environment, exact commands, deviations
and commit hashes. CI/ci-ok, FVM CLI wiring and FVM-to-FEM cross-comparison are
**未执行** in this local library-only delivery.
