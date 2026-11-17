# S2 implementation decisions and deviations

The implementation follows `/workspace/briefs/s2-run.md`, the revised design,
and the supplied concrete plan. Recovery preserved commit `c78b202` and all
interrupted working-tree changes in WIP commit `3a16266`, then fetched and
merged `origin/main` (O2 #50) with a merge commit, without rebasing. The merged
solver retains O2's shared material M7 adapter (tested against `CopperMixture`),
PETSc CSR preallocation, six-DOF rigid-anchor validation and Q2-to-Q1 output
interpolation. S2 extends those paths for cut integration and output. Upstream
Kokkos mesh-test initialization and the Q2 output regression are retained.

Mesh geometry and quadrature remain authoritative. The solver uses the mutable
clipper to compact exterior cells/nodes while preserving GIDs; its cut indices
and polyhedron parent indices refer to the resulting compacted background
arrays. Attributes continue to identify materials and layers. The clipper
prepares `GhostPenaltyCache`; no second GID numbering or quadrature generator
is introduced. Additional mesh-owned surface rules cover surviving original
faces for convection and cut-top PV samples. Explicit EB triangles replace the
old preview writer's degree-2-specific reconstruction from quadrature triples.

The application pipeline is the existing implemented solver. The unrelated
`ThermalStressSolver::advance()` placeholder remains unchanged. Reusable cut
rule adapters and ghost face kernels live in `solvers/fem`; application adapters
handle loads, constitutive conversion and results. MFEM invokes local kernels
per cell/face, each writing its own dense matrix/vector; global insertion is
outside those kernels. Distributed/GPU assembly is not introduced. The replicated
PETSC_COMM_SELF backend visits each interior face once per independent solve,
so rank ownership is used for output rather than splitting a global assembly.

Supported case geometry is a convex extruded polygon, applied to all layer
bodies as one external footprint. Non-convex outlines, holes, general solid
imports and internal polygon material cuts fail explicitly. The geometric and
manufactured sphere tests use increasingly fine convex plane approximations;
there is no claim of exact curved-sphere integration. The rotated planar box is
represented exactly, so its geometric errors are checked against roundoff
rather than requiring strict decreases of roundoff noise. Natural embedded
conditions are implemented; embedded essential conditions explicitly reject
input because Nitsche is absent.

Natural manufactured solutions are compared modulo their constant or rigid
nullspace, with weighted L2 projection. Four non-dyadic h levels (5,7,9,13)
exercise changing cut fractions. Geometry tests compare outward-ring divergence
volume with mesh polyhedron volume, and FEM independently compares integrated
volume to that volume, both at 1e-12. Degrees 4/6/8 all meet the requested orders;
6 is retained as the default.

The robustness test uses direct symmetric dense eigenvalues for its small Q2
matrices, rather than PETSc/Lanczos. It records unscaled and Jacobi-scaled
condition numbers, enabled and zero penalty, thermal and elastic systems, 50
translations, homogeneous/two-material stacks and both unit and 1:10 cells.
The factor-of-ten acceptance applies independently to unscaled and Jacobi-scaled
matrices, with the original gamma=0 comparison. The body has fixed width 1 and
translates through one background-cell period inside a width-1.5 background
with h_x=h_y=0.5. Two opposed cut planes move together; an integrated-volume
assertion enforces an unchanged physical domain volume at every position.
The y=0 face grounds heat and clamps displacement, so the grounding distance
stays fixed under x translation. The former one-plane scan grew the body from
width 0.5 to 0.99 while clamping x=0, confounding changing physical length with
cut-position sensitivity. Mesh-owned cut integration and ghost-face assembly,
material scales, gamma=0/0.1, all 50 positions and the sub-1e-6 cut check remain.
On every mesh-provided same-material ghost face, the existing k=1,2 normal
jump terms are augmented by `gamma * scale / 2` times the integral over both
background cells of the squared full-gradient jump of their polynomial
extensions. This symmetric positive-semidefinite term controls tangentially
varying Q2 sliver modes and vanishes for a common Q2 polynomial. The affine
axis-aligned tensor-cell mapping extends each polynomial across the full patch;
no cut-fraction-dependent coefficient or basis scaling is used. The material
scale remains kappa or 2*mu+lambda. Across material interfaces the original
flux-weighted thermal and curvature-only elastic terms remain, since extending
full gradients there would penalize physical flux/thermal-strain jumps.
Zero-penalty numerically nonpositive
minimum eigenvalues are reported as infinite condition estimates, rather than
as finite trustworthy estimates.

The same-domain bilayer reference is an uncut, conforming H1 Q2 wedge mesh built
from planar footprints. This substitutes a fitted wedge mesh for a fitted hex
mesh: slanted polygon corners do not admit the original Cartesian hex layout.
The comparison uses identical physical sample positions for each pair of
solutions. PV errors at intermediate refinements are non-monotone; the test
requires every error below 1% and finest error below coarsest, and records the
intermediate measurements. No ersatz degradation is used.

Existing O2 pipeline checks remain unchanged; the mesh default-bit test compares
with explicit degree 6. The merged O2 test verifies its constitutive bridge against
`CopperMixture`. The output adapter uses the existing results writer, extending
its previously empty EB block to carry mesh-owned triangles and Q2 fields.
The writer remains VTKHDF 2.8 with outward rings copied per cell.

The packaged example is a synthetic two-material cut-corner stack, substituted
for an importer-generated package. GPU/material CUDA tests remain skipped when unavailable; the newly merged
preview digest comparison now runs. The user instruction
to commit without pushing takes precedence over the brief's push/PR instruction.

Recovery validation also found absolute-temperature cancellation in thin-domain
convection: a constant 350 K solution missed the 1e-8 K check by 1.54e-8 K.
The thermal system now solves for `T - T_ref`, including shifted essential and
convection data, then restores absolute T before mechanics/output. The same
check measures 2.21e-9 K error; layered conduction is within 1.82e-12 K.

Ghost faces store **local element numbers**. Cross-rank ghost faces are not
supported. The cut solve is serial-only (`PETSC_COMM_SELF`); GID lookup is
required before GMG or distributed use. The constructor checks positive,
consistent reference orientation and affine axis-aligned hexahedra.

The translated body has no physical x=0 face. We retain y=0 as a fixed physical
face with invariant clamp distance, and measure a second y=0 constraint set
excluding every DOF supported on a cut cell. Acceptance is decided by the
natural-boundary nonzero spectrum (one constant or six rigid modes removed),
independently for raw and Jacobi-scaled matrices, with the unchanged 10x bound.
The restricted y=0 variant on this 3x2x2 scan fixes only one vertical line;
elasticity retains rotation about that line. Its full condition number is
infinite. We additionally report its nonzero spectrum after verifying and
excluding this one remaining rigid mode; this does not replace the decisive
natural-boundary test or the original fully clamped test.

Signoff measurements on the merged release build retain the volume coefficient:
the natural-boundary condition-number variation is at most 5.09671 across the
50 positions (both spectra must satisfy 10). At bilayer n=12, changing gamma
from 0.01 to 0.1 changes PV by 0.0458007%; the n=8 S2 application example
changes by 0.0154750%, both below the unchanged 0.5% regression bound. The
example prescribes uniform 300 K, so T_max is 300 K at all three gamma values.
Bilayer output now includes each layer's top-surface PV against the fitted
reference; manufactured output includes successive-level orders as well as
the original regression-fit order assertions.
