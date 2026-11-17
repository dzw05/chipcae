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
translations and both unit and 1:10 cells. The factor-of-ten acceptance applies
to Jacobi-scaled matrices, matching the diagonal basis normalization in the
production direct solver. Raw estimates are recorded separately; no factor-of-ten
claim is made for the unscaled basis. Zero-penalty numerically nonpositive
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
