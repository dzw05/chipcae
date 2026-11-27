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

Nightly `cuda-solve-compile` is configured to compile the solver, FEM and all
`tests/solve` executables with CUDA 12.6, CUDA-enabled MFEM and CPU PETSc/Hypre
on a hosted runner without a GPU. Hosted CUDA compilation is **not yet verified
(未实测)**; no first green run has been recorded. Local acceptance checks the
compile-fault switch using a test-only header, which does not validate CUDA MFEM.
GPU execution and acceleration remain **未实测**. The nightly job does not run
solver or test binaries; dependency configuration uses native CPU feature probes.
The optional dispatch input `inject_cuda_fault` checks that the CUDA branch fails
compilation and defaults to false. Selecting the global MFEM device may expose
host/device memory incompatibilities in the existing PETSc solve path.

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
FVM accepts `jacobi` and `gamg` (default). FEM stabilization and penalty
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

## direct 求解的 rtol 含义

当 `preconditioner=direct` 时，`solver.linear.rtol` 是物理（未缩放）系统的
Oettli–Prager 分量后向误差上限：

```text
r = b - Ax
omega = max_i |r_i| / (|A||x| + |b|)_i
eta = ||r||_inf / (||A||_inf ||x||_inf + ||b||_inf)
```

`omega <= rtol` 表示返回的 x 是邻近系统 `(A+ΔA)x=b+Δb` 的精确解，扰动满足
`|ΔA| <= omega |A|`、`|Δb| <= omega |b|`。零分母且零分子记 0，否则记正无穷；
非有限计算拒绝接受。`eta` 是 Rigal–Gaches 范数后向误差，仅作报告。
`rel=||r||_2/||b||_2` 也仅作报告，零 RHS 时为 `inf`；全零解的 omega、eta 为 0。
迭代法（jacobi、gamg、column_gmg）的 rtol 仍表示相对残差。
前向误差估计约为 `cond(A,x) * omega`，病态系统仍可能有较大的解误差。

初次 LU 解后即评估误差，随后复用同一分解求物理残差的修正。omega 不大于
`u=epsilon/2`、较上次下降不足一半，或评估次数达到 `min(max_iter,8)` 时停止。
此上限包含初次评估，因此最多进行 7 次修正；不以 omega 达到 rtol 提前停止。
停止后恢复所有已评估解中 omega 最小的解，以其 omega 判定接受。保存最优解
是相对 LAPACK xGERFS 停止规则的扩展。Residual 事件仍使用 `method="PETSc.LU"`，
其 residual 值仍为 `||r||_2/max(||b||_2,1)`，使用最终返回的 x 计算。
日志中的 omega 和诊断 rel 与此字段分别解释。计时日志另报 eta、rel、refine_iterations 和
refine_seconds，后者包含误差评估、修正求解及最优解维护。

旧判据 `||r||_2/max(||b||_2,1)` 的舍入下限约为
`u ||A||_inf ||x||_inf / max(||b||_2,1)`，且分母中的 1 与物理 RHS 量纲不一致。
此前诊断实测：trace-map 的 rel 为 `1.54e-13` 至 `2.54e-13`、omega 为
`2.6e-16` 至 `7.0e-16`；board kz2 的热场 rel 为 `1.49e-12` 至 `2.42e-12`、
omega 不超过 `5.5e-16`，力学 rel 为 `2.48e-12`、omega 为 `8.6e-16`。
这些 rel 下限会误拒绝已有很小后向误差的解。rtol 数值及默认值 `1e-8` 未调整。
这是 direct 内部接受判据的缺陷修复，按用户给定的 rtol 检查 omega，不放宽 rtol。
公开签名、schema 和结构化事件字段的含义均不变。

出处：

- W. Oettli & W. Prager, *Numerische Mathematik* 6 (1964), 405–409；
  N. J. Higham, *Accuracy and Stability of Numerical Algorithms*, 2nd ed., SIAM (2002),
  Thm 7.3、§7.2（分量后向误差及前向误差估计）。
- J. L. Rigal & J. Gaches, *J. ACM* 14 (1967), 543–548；Higham Thm 7.1（范数后向误差）。
- M. Arioli, J. W. Demmel & I. S. Duff, *SIAM J. Matrix Anal. Appl.* 10(2) (1989),
  165–190；LAPACK xGERFS（精化停止规则）。

## Q2 validation export

Set `CHIPCAE_ACCURACY_Q2_EXPORT=1` to export the FEM solution's complete Q2 nodes for
validation. Only the exact value `1` enables this output; it is off by default. Each rank
writes `<out>/accuracy-q2-rank<rank>.json`, next to `manifest.json`, including an empty
`{"elements":[]}` file when that rank owns no elements. A completed file replaces its
same-directory `.tmp` file only after writing and closing successfully.

The JSON has exactly one key, `elements`. Each element has `gid` (the existing integer
GID), `points` (27 × 3 coordinates in m), `U` (27 × 3 displacements in m), and `T`
(27 temperatures in K). Elements are strictly increasing by numeric GID and appear only
on their owning rank. Matching indices in `points`, `U`, and `T` describe the same node.
Nodes follow `q = i + 3*j + 9*k`, where `i,j,k = 0,1,2` select reference coordinates
`ξ,η,ζ = 0,½,1`; x varies fastest on the Cartesian mesh. All 8 vertices, 12 edge
midpoints, 6 face centers, and the cell center are included, with doubles serialized at
17 significant digits for exact round trips. Thermal-only runs export zero `U`.

Cut cells export all 27 nodes of the complete background hexahedron, including extension
nodes outside the physical domain; their values are the background FE polynomial evaluated
at those nodes. Deleted exterior cells are excluded. Tensor Q2 evaluation can therefore
recover the FE solution anywhere within the retained physical part of a cut cell.

This optional sidecar leaves the manifest and VTKHDF format unchanged. Expect roughly
4–5 KB per cell, or approximately 0.5 GB for 100,000 cells. Export streams one cell at a
time and logs local `q2_export elements=… nodes=… bytes=… seconds=…` metrics through the
existing event logger (persisted on rank 0).

## K1 stop measure (CHIPCAE_K1_STOP)

`CHIPCAE_K1_STOP` is an internal switch, off by default. Unset or `legacy` keeps the
existing direct solve and timing output. The exact value `ir` enables mixed precision
iterative refinement for the displacement field U when the selected preconditioner is
`direct` on a structured (non-clipped) mesh. Clipped meshes retain legacy refinement
and timing output even when `ir` is selected. Other values, including an empty string
or `IR`, fail with
`CHIPCAE_K1_STOP must be unset, legacy or ir`.

The acceptance gate is the explicitly recomputed binary64 residual
`rel_d = ||b-Ax||₂ / ||b||₂ <= 1e-12`, with `atol=0` and no residual floor. Products
and sums follow CSR row order without implicit FMA contraction. Correction residuals
use portable double-double accumulation (Ogita–Rump–Oishi Dot2, Knuth TwoSum and explicit
`std::fma` TwoProduct); the solution, factors and corrections remain binary64. No
extended `long double` precision is required. Zero numerator and denominator give a
zero ratio; a nonzero numerator over zero gives infinity, including for the diagnostic
estimates.

Refinement reuses the existing scaled PETSc LU factors. A zero Dot2 residual stops
immediately; otherwise stagnation is checked after at least two corrections, using
`rel_e > previous_rel_e/2`, with at most nine corrections. Satisfying the gate alone
does not stop refinement. The smallest binary64 relative residual is retained, with
later iterates winning ties. Nonfinite inputs, intermediates or statistics invalidate
the solve. Invalid results or a retained residual above the gate fail with
`K1 stop measure failed: ...`; there is no fallback to legacy.

The existing U timing line gains `k1_stop`, `rel_d`, `rel_e`, `est_inf`, `est_abs`,
`ir_corrections`, `gate`, `atol`, `gate_precision=binary64`, `gate_order=csr_row`,
`residual_precision=double-double` and `k1_ir_seconds`. The estimates and backward
errors are diagnostics only. Structured Residual events retain their existing norm
and the result formats remain unchanged. T and column_gmg (including its direct
fallback) retain their existing refinement.

The intended scope is the structured Q2 direct solve on synthetic boxes with 2–16
cells per layer (up to about 44,000 displacement DOFs), and manufactured unit systems
up to 10,000 unknowns. No accuracy claim is made for kz >= 32 or iterative solvers.
