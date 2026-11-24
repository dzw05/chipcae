# Q2 thermal stress solver

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
plane and harmonic through thickness. Polygon copper maps and internal material
cuts are rejected.

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
其 residual 值现在是 omega。计时日志另报 eta、rel、refine_iterations 和
refine_seconds，后者包含误差评估、修正求解及最优解维护。

旧判据 `||r||_2/max(||b||_2,1)` 的舍入下限约为
`u ||A||_inf ||x||_inf / max(||b||_2,1)`，且分母中的 1 与物理 RHS 量纲不一致。
此前诊断实测：trace-map 的 rel 为 `1.54e-13` 至 `2.54e-13`、omega 为
`2.6e-16` 至 `7.0e-16`；board kz2 的热场 rel 为 `1.49e-12` 至 `2.42e-12`、
omega 不超过 `5.5e-16`，力学 rel 为 `2.48e-12`、omega 为 `8.6e-16`。
这些 rel 下限会误拒绝已有很小后向误差的解。rtol 数值及默认值 `1e-8` 未调整。
这是 direct rtol 对外语义的变化，不改变冻结契约字段。

出处：

- W. Oettli & W. Prager, *Numerische Mathematik* 6 (1964), 405–409；
  N. J. Higham, *Accuracy and Stability of Numerical Algorithms*, 2nd ed., SIAM (2002),
  Thm 7.3、§7.2（分量后向误差及前向误差估计）。
- J. L. Rigal & J. Gaches, *J. ACM* 14 (1967), 543–548；Higham Thm 7.1（范数后向误差）。
- M. Arioli, J. W. Demmel & I. S. Duff, *SIAM J. Matrix Anal. Appl.* 10(2) (1989),
  165–190；LAPACK xGERFS（精化停止规则）。
