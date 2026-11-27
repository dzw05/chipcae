# Cell-centered thermoelastic finite volumes

The library accepts replicated control volumes, oriented faces, engineering stiffness
matrices and thermal strains. It has no CLI or temperature-solver dependency. The caller
owns MPI/PETSc initialization and constructs materials with the existing
`material::copper_laminate` compliance adapter. All public quantities use SI units.

The parent baseline is commit `40fa721322bbe65982a9f4f17b7c73251e1f7c21` (#207).
It uses symmetric cell-strain energy plus compact face stabilization and originally
rejected embedded faces. This extension supports cut and concave polyhedra, with
traction-free embedded faces. Empty or all-zero `is_cut` preserves the parent uncut
arithmetic; otherwise the flags must be binary and have one entry per cell.

`ElasticMesh::gid` is a unique external key. Sparse, descending or interleaved gids are
valid. Internal permutations sort by owner rank and gid; results keep caller cell/face
order on every rank. Identical prescribed face values merge; conflicting values fail.
Every Fixed side/layer target must match a nonembedded exterior face. A layer target
fixes all exterior faces of that layer, including lateral faces. Errors reuse
`mesh::Expected` and `mesh::Error`; invalid metadata reasons start with `INPUT_INVALID`.
Invalid cell volume, face area, centroid or distance geometry uses `INTERNAL`, including gid and face index where
applicable. No numeric error-code member or new error carrier is introduced.

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
full stiffness and thermal eigenstrain. On uncut, noninterface faces, a face energy penalizes the
compact-gradient residual `deltaU-Gbar*d` using the harmonic acoustic tensor and `area/(n.d)`. This
vanishes for affine fields and interior quadratic bending fields on the parent grids.
The geometry is supplied by the caller, including cut polyhedron moments.

An internal face enters the interface path exactly when neither side is cut,
`ElasticProblem::region` is nonempty, and `region[lo] != region[hi]`.
Region ids are compared only for integer equality; they may be negative or sparse.
An empty region vector means a single region; any other length than the cell count
reports `INPUT_INVALID` with both lengths before assembly. Neither layer labels nor
numeric differences in stiffness, alpha or temperature select interfaces. With no
qualifying faces, original assembly and output arithmetic is retained byte for byte,
including heterogeneous data and smooth cellwise temperatures. CLI callers leave
region empty in this change; pipeline wiring belongs to the separate D2-wire card.
The bypass keeps the legacy gradient vector and assembly/solve call sequence;
interface lookups are confined to the interface reconstruction instantiation.

For an interface, orient `n` from lo to hi and define positive normal distances
`d_lo=(x_f-x_lo).n`, `d_hi=(x_hi-x_f).n` and separate tangential offsets
`Delta_lo=x_f-x_lo-d_lo*n`, `Delta_hi=x_f-x_hi+d_hi*n`. Let `T_s G` be the
engineering constitutive traction, `K_s=T_s(outer(I,n))` the acoustic tensor,
and `P=I-outer(n,n)`. The shared tangential gradient uses **K**, not K/d, weights:
`G_t=(K_lo+K_hi)^-1 (K_lo*G_lo*P+K_hi*G_hi*P)`.
With `b_s=T_s G_t-thermal_traction_s`, traction continuity gives

```text
H U_f = (K_lo/d_lo)(U_lo+G_lo Delta_lo)
      + (K_hi/d_hi)(U_hi+G_hi Delta_hi) + b_hi-b_lo,
H = K_lo/d_lo+K_hi/d_hi.
```

Each one-sided LS fit replaces every cross-interface center sample by that face's
own unknown displacement at its face centroid, retaining same-material centers,
prescribed face samples and free-face constitutive closure. Write these fits as
`G_s = a_s + sum_g B_sg U_g` and substitute into the traction equations. Cholesky
solves apply H's inverse to every affine coefficient and constant. Sparse block
elimination uses a three-variable local SVD at each face, substitutes its map into
dependent face equations, then back substitutes to resolve all interface displacements.
No connected-interface dense matrix or inverse is allocated. The existing
one-sided Jacobi SVD retains squared singular values strictly greater than
`largest*1e-26`, with unchanged arithmetic on the legacy path. Incomplete gradient
or elimination rank and nonpositive acoustic Cholesky pivots report `INTERNAL`
with the lo gid and face index. Zero projected distances also report `INTERNAL`;
negative or nonfinite projected distances fail input validation with `INPUT_INVALID`
before interface construction. There is no interpolation
fallback. At a junction, the resulting cell gradient includes all incident
interface samples and is shared by
ordinary reconstruction, interface tractions and half-face energies. Sparse
elimination can introduce fill along coupled interface maps; compact input fits
do not imply a one-ring eliminated stencil.

The same eliminated U_f operator enters both the cell Green–Gauss energy and the
returned face displacement. Interface stabilization consists of two half-face
energies `0.5*(A_f/d_s)*r_s^T K_s r_s`, with
`r_s=U_f-U_s-G_s*(x_f-x_s)`. For a compatible piecewise-affine field, each local
fit and U_f reproduce the field, both residuals vanish, and the cell energy has
balanced interface traction. Differentiation of the complete affine energies
retains symmetry and all six rigid modes without modifying K or solver settings.
Equal region ids bypass this path regardless of constitutive or thermal data.
Different regions use it even when their C and alpha agree. With equal acoustic
tensors K, the equation reduces to
`U_f=w_lo*(U_lo+G_lo*Delta_lo)+(1-w_lo)*(U_hi+G_hi*Delta_hi)
+d_lo*d_hi/(d_lo+d_hi)*K^-1*(b_hi-b_lo)`, where
`w_lo=d_hi/(d_lo+d_hi)`. The shared tangential gradient makes its constitutive
contribution cancel for equal C; the remaining b difference is the negative
thermal-traction jump. A smooth temperature has an O(h) cell-to-cell jump, so the
O(h) distance factor makes this correction O(h^2). On an aligned face with equal
eigenstrain it vanishes, leaving the geometric interpolation. On skew faces,
separate offsets need not equal the legacy common-offset formula for unequal
gradients; their smooth-field difference is also O(h^2). Byte invariance therefore
follows from the region bypass, rather than a numerical material comparison.

Only the lo owner constructs each interface equation and its LS maps. Construction
errors are reduced before any coefficient broadcast; ordered integer keys and
floating-point coefficients are then replicated separately. Each rank eliminates
the coupled maps in the same face order, with failures synchronized before further
assembly. Records are sparse, cell gradients are stored once, and temporary
elimination and duplicated gradient storage are released before PETSc allocation.
This does not guarantee unchanged matrix support or peak RSS: stiffness contrast
can enlarge the implicit stencil.
The private interface diagnostic evaluates the common tangential gradient, signed
half-face normal derivatives and one-sided constitutive tractions from these same
maps. Cut faces, including either adjacent cut flag, always retain the cut path;
material jumps on cut faces remain outside this change.

For cut cells, face values use inverse Euclidean cell-to-face distances to weight
the two owner extrapolations. Prescribed boundaries use their supplied values; free
and embedded boundaries use the owner extrapolation. The cut gradients satisfy the
coupled equations `G_c = sum_f U_f tensor S_cf / V_c` fully implicitly. Noncut
least-squares operators remain unchanged. Reconstruction and assembly share the
same face-value operator, including embedded top samples.

The Green–Gauss equations alone do not define a unique gradient for every allowed
boundary layout. The explicit completion used here minimizes the Euclidean change
from the parent's gradient/constitutive closure, subject to the Green–Gauss constraints.
For cut cells whose local displacement samples do not span space, the prior closure
adds center samples along the connected face graph until they span space or the body
is exhausted. This makes terminal cut cells reproduce rigid rotations; the one-ring
constitutive closure alone cannot identify rotation about a lone neighbor direction.
Twice-reorthogonalized row elimination retains independent constraints
and checks redundant equations for compatibility. This is a constrained projection,
rather than a pseudoinverse of an underdetermined Green–Gauss system; it retains the
parent closure only in unconstrained directions. The all-prescribed single-cell
case is defined entirely by the face sum, while partially prescribed and all-free
cells have a defined completion. The dense constraint elimination uses quadratic
storage and cubic work in the number of cut cells; it is a correctness path for
modest replicated meshes, not a scalable cut-mesh implementation.

Before any gradient assembly or KSP solve, every face contributes
`rho = (d dot n)/|d|`, with `d = C_hi-C_lo` internally and `C_face-C_lo` on every
boundary. Zero/nonfinite distances fail. A cut face (either adjacent flag or embedded
status) requires `rho > 1e-6`; its individual owner-to-face projections may be negative.
The strict finite minimum over all faces is reduced first as a double, then the
smallest exactly tied lo gid is reduced as an integer. Cut cells require normalized
face-moment identity defect at most `1e-10` and area-vector closure at the same
relative scale. `max_identity_defect` is the global maximum over marked cut cells.

The cut face-gradient operator evaluates
`G_face = Gbar + (deltaU-Gbar*d) tensor n/(d dot n)`, with the arithmetic mean of
both cell gradients internally and the owner gradient on boundaries. Production assembly
and diagnostic evaluation use the same affine face-gradient operator. On internal and
prescribed cut faces, let `Hbar` be the arithmetic mean of the two cell-energy Gauss
gradients (the owner gradient on a boundary). The cut face energy is
`area*(d dot n)/2 * e^T C_face e`, where `e = engineering(G_face-Hbar)`.
Thus the full corrected face gradient enters the constitutive stress and its adjoint
traction, including tangential differences between reconstruction and cell-energy
gradients at cut/uncut interfaces. The parent cell-energy consistency term already
accounts for `Hbar`; subtracting it avoids counting that strain energy twice. Thermal
eigenstrain cancels in this difference and remains in the parent cell energy.
When the reconstructed and cell-energy gradients agree, this reduces algebraically
to the acoustic compact penalty. Free and embedded boundary tractions remain zero.
Face material interpolation retains the componentwise harmonic/mixed formula below,
but uses positive Euclidean
distances on cut faces; uncut projected weights retain their original ordering.

**Design deviations from the brief/plan:** the full implicit option replaces delayed
correction (`gradient_corrections = 0`). The parent variational assembly is retained:
cell energy and the full cut face-gradient mismatch energy are differentiated once.
The resulting numerical traction includes the face-gradient constitutive stress
difference and the derivatives of both gradient reconstructions, retaining the parent
energy-conjugate traction formulation. It is not simply `C:strain(G_face)` times the
normal; those adjoint contributions preserve the right and transpose rigid modes.
The constrained completion and Euclidean cut material weights are explicit local
design decisions; the available architecture exemption addresses interface scope
and does not supply a discretization ruling. These departures require review as
numerical design choices, even when the acceptance suite passes.

Differentiating these energies gives symmetric block-coupled equations. Differences
are routed through deterministic shortest paths in the local face graph to retain
one oriented integrated traction per physical face, inserted with opposite signs in
its adjacent cells. This enforces force and moment conservation together. Prescribed
boundary reactions follow the same energy derivative; free face tractions are zero.
The complete implicit stencil is preallocated, including beyond second-ring columns
introduced by the adjoint gradient reconstruction.

The retained parent energy-conjugate traction differs from a bare direct
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
call would override it with coordinate-scaled modes. Fixed problems construct no rigid
basis and attach neither exact nor near rigid nullspaces. Empty owner ranks participate in
all collectives and receive the same near-nullspace mode count.

Solutions are gathered once, restored to caller order and reconstructed identically on
every rank. Prescribed face outputs are exact. Internal faces interpolate reconstructions
from both sides. `U_mag_max` uses cell centers. Top samples use nonembedded exterior
side 5 faces and embedded faces satisfying
`normal_z > 0.99` and `centroid_z >= z_top-1e-13`;
a centered, scaled rank-revealing plane fit removes tilt. Empty, collinear
and single-sample sets are supported. Method strings are:

- `FVM cell centers`
- `FVM top-face centroids; least-squares plane removal`

## Reproduction

This subdirectory requires `CHIPCAE_ENABLE_MFEM AND MFEM_USE_PETSC`. Test executables
additionally require `CHIPCAE_BUILD_TESTS`. The thermal card, or a separate lead-owned
integration patch, must add the following conditional block to the parent
`solvers/fvm/CMakeLists.txt`; that parent change is outside this card's scope:

```cmake
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/elastic/CMakeLists.txt")
    add_subdirectory(elastic)
endif()
```

Until integration lands, a separate local CMake harness can add the repository's
`core/` and `solvers/fvm/elastic/` subdirectories with the same dependency gates.
The verification for this change uses that harness without editing the parent.
It covers this library, not a top-level whole-repository build. The normal integrated
commands are:

```sh
cmake -S . -B build-fvm-elastic -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCHIPCAE_ENABLE_MFEM=ON -DCHIPCAE_ENABLE_MPI=ON \
  -DMFEM_DIR="$MFEM_DIR" -DCHIPCAE_BUILD_TESTS=ON
cmake --build build-fvm-elastic --target chipcae_test_fvm_elastic chipcae_test_fvm_bimetal chipcae_test_fvm_elastic_concave -j2
ctest --test-dir build-fvm-elastic -R '^solve_fvm_' --output-on-failure
cmake -S . -B build-fvm-elastic -DCHIPCAE_FVM_ELASTIC_BENCHMARK=ON
ctest --test-dir build-fvm-elastic -R solve_fvm_elastic_benchmark -V
```

Acceptance tests use synthetic tensor grids, the seven-cell concave L with both
separate L/gid1 faces, disconnected cut cells and an independently clipped slanted
4x4x2 grid. Thin-arm and malformed-geometry cases verify rejection before KSPSolve.
The slanted affine patch has stress compatible with the embedded free plane.
Physical KSP operator checks
require exact, near and transpose rigid nullspaces for Rigid321 and their absence for
Fixed, with both preconditioners on one and two ranks. Homogeneous, bimetal and
orthotropic laminate assemblies must also satisfy stiffness symmetry, internal-force
cancellation and right/transpose rigid-mode compatibility. Timoshenko tests use
nu=0 plies to isolate the beam reference, three factor-two refinements, four cells per
ply initially and central-half curvature fits. The orthotropic CLT test imposes
plane-strain kinematics on lateral y faces; it does not infer plane strain from free
finite-strip boundaries. Assertions remain active in Release builds.

## References and limits

Method references from the brief: Demirdzic & Muzaferija, CMAME 125 (1995), 235–255;
Jasak & Weller, IJNME 48 (2000), 267–287; Cardiff, Tukovic, Jasak & Ivankovic,
IJNME 108 (2016), 1572–1592. PETSc documents right/transpose nullspace compatibility under
[MatSetNullSpace](https://petsc.org/release/manualpages/Mat/MatSetNullSpace/).

This is linear, small-strain elasticity with axes-aligned thermal expansion tensors; no contact,
plasticity, nonlinear geometry, vertex reconstruction or FEM comparison is included.
The full replicated mesh and implicit second-ring gradient operators consume memory
on each rank. Componentwise harmonic interpolation can reject an SPD pair if its
interpolated face tensor loses positive definiteness.

## Historical parent Release verification

**实测**, optimized Release with runtime acceptance checks, PETSc 3.26.1:

| Acceptance | Result |
| --- | --- |
| Uncut elastic patches, errors, supports, reconstruction and embedded-input rejection | 110 cases pass |
| MPI np=2, both supports/PCs, zero-owned-row ranks and embedded-input rejection | 10 cases pass |
| Signed Timoshenko heating, 512/4096/32768 cells | errors 2.2741%, 0.5590%, 0.1392% |
| Timoshenko observed orders | 2.0243, 2.0060; cooling gives the same convergence |
| Orthotropic f=0.3 plane-strain CLT | signed curvature 0.362605596231; relative error 2.16e-12 |
| Matrix symmetry defect, homogeneous / bimetal / laminate | `4.69e-17` / `6.70e-17` / `5.31e-17`, required `||A-A^T||F/||A||F <= 1e-13` |

The **实测** 68x68x6 benchmark has 27,744 cells and 83,232 displacement unknowns:

| Base PC / Krylov method | solve_elastic wall (s) | Iterations | Independent `||A U-b||2` (N) | Reported residual (N) | Process peak RSS (KiB) |
| --- | ---: | ---: | ---: | ---: | ---: |
| Jacobi / CG | 32.1559 | 3446 | 2.66786e-8 | 2.66695e-8 | 400580 |
| GAMG / flexible GMRES(50) | 26.1867 | 693 | 1.62500e-8 | 1.62460e-8 | 590368 |

The RHS norm is 7572.82 N. Wall times include validation, assembly, the coarse space,
KSP and reconstruction on a shared build box. RSS uses cumulative process `ru_maxrss`
across both cases and the intervening residual diagnostics. Physical residuals use the
original, unprojected load. The documented backward-error floor was active.

These are historical parent measurements, not new performance measurements of the
cut path. The uncut acceptance tolerances and saved parent fields are retained.
CI/ci-ok, FVM CLI wiring, full-repository integration and FVM-to-FEM comparison are
**未执行** in this local library delivery.

## Historical cut-concave Release verification

**实测**, Release with warnings-as-errors, MPI and PETSc-enabled MFEM, using an
external harness (the parent CMake file remains unchanged): all 6 registered CTests
pass. Four are repository tests; two are local parent-snapshot/geometry helpers.
The repository suites report 113 elastic, 52 concave, 21 np2 and 4 bimetal cases,
with no failures. The saved uncut parent fields pass the `1e-14` relative comparison,
and empty versus all-zero flags produce exactly equal fields and solver diagnostics.

| Geometry | Minimum rho | lo gid | Maximum reported cut moment defect |
| --- | ---: | ---: | ---: |
| Seven-cell L | 0.55708601453115558 | 0 | 0 |
| Slanted 4x4x2 | 0.60414231309573696 | 3567 | 2.4424906541753444e-15 |
| Filled-notch L | 0.70710678118654757 | 0 | 3.3306690738754696e-16 |
| Thin-arm negative | -0.40273861426601698 | 73 | rejected before KSP |

Both cut matrix tests retain all six right and transpose rigid-mode assertions and
opposite internal-force checks. Measured symmetry defects are `9.84e-17` (L) and
`8.40e-17` (slanted). Fully implicit `gradient_corrections` is zero. The L affine patch
and free expansion require zero Krylov iterations after the parent warm starts;
the slanted affine patch requires one, and slanted expansion requires zero.
The independent normalized physical-residual values are recorded in the external
verification log. Jacobi/GAMG and np1/np2 comparisons, including empty-owner ranks,
meet the original `1e-8` relative assertions. Exact rho ties and unequal near-ties use
gids above `2^53` and pass without storing gids in floating point.

Two additional checks cover a free slanted rigid-rotation gradient and broken
area-vector closure despite a valid face moment. The gate-edge fixture is centered
before its shear transform to avoid coordinate subtraction losing `epsilon/rho`;
its tangent is rounded upward by one ulp to represent the intended equality case.
The `rho <= 1e-6` gate, geometry thresholds and all existing acceptance assertions
are unchanged. The original test-first commit failed 45 of 50 concave cases; a new
rotation regression also failed before its connected-patch completion was added.

## Connected face-gradient Release verification

The review fix connects the full face-gradient operator to the parent variational
traction through the cut strain-mismatch energy above. An independent scalar-energy
oracle checks matrix and face-traction virtual work for nonaffine L and slanted fields,
with Fixed and free supports, spatially varying laminate stiffness and thermal loads.
Restoring the pre-fix assembly fails all four cut virtual-work checks; both uncut
virtual-work checks pass. No existing tests or tolerances were weakened.

**实测**, Release with warnings-as-errors, the same external harness passes all 6
CTests: 113 elastic, 62 concave, 29 np2 and 4 bimetal cases, plus the saved-parent
snapshot and geometry helpers. Nonaffine Jacobi/GAMG solves agree within `1e-8`
relatively on cells and faces, including split and empty owner ranks. Independent
physical residuals satisfy `1e-10`. Cut symmetry defects are `9.92e-17` (L) and
`8.49e-17` (slanted), with all right/transpose rigid-mode checks retained. Pinned
clang-format 23.1.5 `-i` followed by `--dry-run --Werror` is clean on both touched
C++ files. This verification covers the local library harness, not a top-level build.
