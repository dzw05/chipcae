# F-1130 trace-map CLI acceptance

This gate checks I1 铜层 1.2（2026-11-20） C3/C4/C11 on the unchanged public synthetic
[board_4layer.xml](../ecad/data/board_4layer.xml): a 5 mm square board with
local copper artwork on all four copper layers. T1 stripe validation is
outside this gate. Feature author assignment remains CodeBuddy (`kimi-k3.1-pro`).

CTest names remain `f1130_uncut_cli_e2e` and `f1130_uncut_cli_records`.
The records test also discovers the independent reference self-tests; no new
CTest registration is needed. Passing reference self-tests alone does not
establish product acceptance.

## Inputs and chain

Import uses [material_map.json](material_map.json), mapping `FR4` to `fr4_eq`
and `copper` to `cu`. Imported layer names, order, kind, material and thickness
are compared directly with the existing import golden, without rewriting IDs.
Each Patterned layer must have `material == 'fr4_eq'`, `copper.metal == 'cu'`,
`copper.polygons` with the exact layer name, and no `copper.fraction`.

```text
board_4layer.xml + material map → chipcae-import → stack.json + stack.polyset + defects
unchanged stack.json + stack.polyset → chipcae-preview → preview.vtkhdf + cell_count
same stack/sidecar + case/materials → chipcae-solve ranks=1 → result readback
same input bytes, profile.ranks=2 → MPI chipcae-solve → result readback
independent geometry + Q2 / C3 / C4 / C11 / energy / Wiener / MPI → result records
```

Neither this chain nor the two-layer `chain_ipc_uncut_chain` calls the
layer-average conversion tool. The two-layer test retains its import order,
thickness, FR4 base-material and readback checks, and uses the shared reference
on its own geometry (copper areas 6 and 12 mm²).

Preview uses `--nx 4 --ny 4 --levels 0 --max-ratio 100`. The acceptance case
uses the same mesh parameters and `min_elems_per_layer=1`; no cut cells are
allowed, every hex has `cell_kind=0`, and its count must match preview.
The existing coupled thermal/mechanical loads and 3-2-1 constraints remain:
zmin fixed at 298.15 K, zmax convection with h=1000 W/(m² K) and T_inf=348.15 K.
The direct solver uses `solver.linear.rtol=1e-12`, as prescribed by the
PR #200 architect ruling. The C11 acceptance limit remains 1e-8 K.

Diagnostic exceptions are recorded as failures while preview and both solve
attempts continue. Both solve/readback attempts run before numerical inspection,
so a numerical failure in one rank does not suppress the other attempt. Inspection records C11
failures while retaining the measured temperature error, then measures readback
and returns the exported mesh for the aligned rank comparison. A failed numerical
inspection keeps its numerical completion check false and its record verdict fail.
Stack, sidecar, case, materials and both profiles are checked byte for byte after
each attempt.
Profiles differ only in ranks. Every attempt produces a separate
`chipcae.vv_result/0.2` record with `data_class: synthetic`, `label: 实测`,
`reference.type: 独立代码`, and `reference.value: null`. Missing fraction,
conductivity, physics, numerical or parallel completion checks force
`verdict: fail`. Records must pass `tools.vv.check_result_data_class.check_file()`.
Commands, stage wall times and records are printed before temporary cleanup.

## I1 1.2 consumer expectations

This gate consumes the per-cell coefficients specified by the I1 copper-base
ruling 1.2 (2026-11-20, retaining the Q2 reference) and the producer brief
`solve-copper-trace-map` (M3). Producer implementation and public I/O contract
changes belong to M3 and the I/O owner. This test card changes no public
interface, manifest schema or VTKHDF version.

The gate requires both coefficient fields. For the uncut hex block
with N cells summed over partitions, the reader requires:

| CellData field | dtype | Dataset shape | Components / physical unit |
| --- | --- | --- | --- |
| `copper_fraction` | float64 | `(N,)` | 1 / dimensionless |
| `conductivity_diag` | float64 | `(N,3)` | global xyz diagonal / W/(m K) |

Rows follow cell/connectivity order within each partition and partition
concatenation across the block. Conductivity rows contain `(k_xx,k_yy,k_zz)`;
empty partitions contribute no rows. The gate checks field presence and shape
explicitly; an older O1 result without either field cannot pass F-1130.
These test expectations prescribe neither default writer fields nor manifest
unit metadata. `layer_id` is the 1-based stack index, checked against physical
z membership; non-Patterned cells have zero copper and their material diagonal.
The fixed FR4/Cu formula and numerical tolerances are specified below.

Existing owner approvals of I1 1.1 are recorded in the project handoff. They
are historical approvals of the earlier model. The current conductivity model
follows I1 铜层 1.2（2026-11-20）; this card changes no public I/O contract.

| Owner | Recorded approval (2026-11-19) | Evidence identifier / relevant condition |
| --- | --- | --- |
| mesh (M2) | 13:10 | `handoff/status/mesh.md`: global elem_gid determines columns, independent of rank-local order; polygon areas use union area. |
| solver (M3) | 13:36 | `handoff/status/solver.md`: Q2 reference, 3×3×3/3×3 Gauss and vertex comparison. |
| vv (INT) | 13:20 | `handoff/status/vv.md`: absolute fraction tolerance 1e-12 and volume/thickness area weights for multiple z cells. |
| desktop notification | 09:55 | `handoff/inbox/desktop.md`: both added output fields announced. |

## Independent reference

[trace_map_ref.py](trace_map_ref.py) decodes `CCPOLY01` with `struct`, retaining
integer vertices and a rational DBU scale. It clips signed copper rings to
cell xy rectangles using exact `Fraction` Sutherland–Hodgman intersections
and signed shoelace areas. Conversion to floating point follows area division;
fractions are cached by exact layer name and rectangle. Disjoint simple outer
rings and contained signed holes are supported. Ambiguous overlapping/touching
rings, nonrectangular board outlines and malformed hex topology fail explicitly.
The reference imports no product geometry/solver or conversion-tool helpers
and uses no Shapely.

VTKHDF decoding respects `NumberOfPoints`, `NumberOfCells` and
`NumberOfConnectivityIds`: offsets and connectivity restart in each partition.
The [I1 1.2 consumer expectations](#i1-12-consumer-expectations) above describe
the two required coefficient fields and the recorded owner approvals.
Cells are aligned by globally unique `elem_gid` when exported, with complete
dense Morton column coverage and layer/z membership checked independently.
O1 permits omitting hex `elem_gid`; in that case exact rational cell bounds
provide alignment, with complete, unique coverage and layer/z checks retained.
Dense coverage belongs to this uncut fixture. Vertices are
aligned by rational coordinates; repeated boundary temperatures/displacements
must agree before deduplication. Partition ordering cannot choose the column.

For each copper cell, with independently calculated f, I1 铜层 1.2（2026-11-20）
uses parallel conductivity for all three components:

```text
k_x = k_y = f*390 + (1-f)*0.3
k_z = f*390 + (1-f)*0.3
```

The matrix conductivity is fixed at 0.3 W/(m K), independent of the imported
material identity. Core must have f=0 and diagonal (0.3, 0.3, 0.3).

The thermal reference generates shared 27-node tensor Q2 DOFs from rectangular
hex corners, including edge, face and cell midpoints. Quadratic Lagrange bases
at {0, 1/2, 1} and analytic derivatives use 3×3×3 volume Gauss integration and
3×3 top-face integration. It solves for theta=T−298.15 with the same Dirichlet
and Robin conditions as the case, using SciPy sparse `spsolve` when available
and a NumPy dense direct fallback otherwise. Nonfinite solutions fail.
Only exported corner temperatures enter the C11 comparison.

The current exporter supplies corner temperatures, without full Q2 boundary
values or flux. Energy evaluation therefore pins exported corners and solves
the independent equations for the remaining Q2 DOFs. Q_top integrates top
convection with 3×3 Gauss; Q_bot is minus the summed bottom Dirichlet reactions
from that same assembled operator. This is a **discrete variational flux**.
Every exported corner stays pinned, including non-Dirichlet corners whose
reactions need not vanish. The energy imbalance equals minus their summed
reactions; recovery does not force conservation for an incorrect exported field.
An analytic single-column regression perturbs exported top temperatures by
±1 K and verifies that both recovered fluxes change and energy fails at 1e-9.
Recovery is tested against complete synthetic Q2 solutions. Raw gradient face
flux is not recoverable from corner exports alone; if that interpretation is
required, an architect ruling and additional output are needed. Measured product
energy results are recorded below.

## Acceptance criteria

Every criterion below applies independently to ranks=1 and ranks=2.

| Check | Required result |
| --- | --- |
| C3 cell fraction | Finite, 0 ≤ f ≤ 1, abs(f−phi_ref) ≤ 1e-12 × max(phi_ref, 1), equivalent to absolute 1e-12 for phi_ref in [0, 1] |
| C3 spatial variation | Each copper layer has copper-positive and copper-free columns |
| C3/K1 copper area | F.Cu=0.307835, In1.Cu=2.25, In2.Cu=3, B.Cu=7 mm²; relative error ≤ 1e-12 |
| C4 conductivity | Every diagonal component finite; relative error ≤ 1e-14 |
| C11 temperature | Maximum vertex absolute error ≤ 1e-8 K |
| Energy | abs(Q_top−Q_bot)/max(abs(Q_top),abs(Q_bot),1e-30) ≤ 1e-9 |
| Readback | Manifest warpage from result readback agrees within relative 1e-12 |
| MPI | Aligned cell/vertex fields and existing QoIs agree within `PARALLEL_RTOL=1e-8` |

Copper area is `sum(f_e*V_e)/t_layer`, converted to mm². This avoids counting
an xy column repeatedly when its copper layer contains multiple z cells.
`warpage_pv` and `U_mag_max` must be finite and positive; `T_max` must be finite.
Independent readback `w_bfp_m` is printed as a diagnostic.

Wiener bounds use independent reference coefficients and area-weighted means
within each layer, A=25e-6 m², layer thicknesses in metres and h=1000:

```text
R_lo = sum_l t_l/(mean_l(k_z)*A) + 1/(h*A)
R_hi = sum_l t_l*mean_l(1/k_z)/A + 1/(h*A)
epsilon = 1e-9 * max(abs(50/R_lo), abs(50/R_hi), abs(Q_bot), 1e-30)
50/R_hi - epsilon <= Q_bot <= 50/R_lo + epsilon
```

Resistance is in K/W and heat flow in W. Volume weights implement the same
layer area means when the z discretization has multiple cells.

Two reference diagnostics print before product numerical assertions, without
acceptance assertions or a conversion-tool call:

- `trace_map_vs_layer_average_dT_max=`: maximum full-Q2 nodal difference between
  independent 4×4 trace and layer-average references on the same grid.
- `trace_map_nx_double_dT_max=`: delta-T maximum at 8×8 minus that at 4×4, using
  all Q2 DOFs and a 298.15 K baseline. The synthetic doubled-grid fallback
  preserves the original z axes.

## Running and reference self-tests

Use plain build/test command names with the shared queue shim first on PATH;
allow slot waits and use at most two jobs. Supply installed dependency paths
through CMake options as needed. Python 3.10+, NumPy and h5py are required;
SciPy is optional.

```sh
cmake -S . -B "$build" -DCMAKE_BUILD_TYPE=Release \
  -DCHIPCAE_BUILD_TESTS=ON -DCHIPCAE_ENABLE_ECAD=ON \
  -DCHIPCAE_ENABLE_JSONCONS=ON -DCHIPCAE_ENABLE_MFEM=ON \
  -DCHIPCAE_ENABLE_PETSC=ON -DCHIPCAE_ENABLE_MPI=ON -DCHIPCAE_ENABLE_HDF5=ON
cmake --build "$build" -j2
ctest --test-dir "$build" -V \
  -R '^(f1130_uncut_cli_records|f1130_uncut_cli_e2e|chain_ipc_uncut_chain)$'
ctest --test-dir "$build" --output-on-failure -j2
python3 -m unittest discover -s tests/integration/f1130_uncut_cli \
  -p test_uncut_cli_e2e.py -v
```

CMake supplies executable locations when ECAD, MFEM, MPI and HDF5 are enabled.
Missing executable configuration returns the existing skip code 77. With
backends configured, only an ImportError from h5py returns 77, printing exactly
`SKIP: h5py missing (pending infra-ci-h5py)` to stderr. Missing NumPy and
non-ImportError dependency failures fail acceptance. HDF5-dependent record
cases use `self.skipTest('h5py missing (pending infra-ci-h5py)')`; CTest
reports their unittest process as Passed even when its output says `skipped=`.
Configured command failures fail acceptance. Commands have 600 second
timeouts. A missing MPI launcher fails, and no retired fixture gate suppresses
configured acceptance.

The records discovery path includes 13 assertion/verdict tests and
17 reference tests: exact clipping, signed holes and empty copper, binary
validation and exact fixture areas, Q2 partition/derivative sums (1e-14),
symmetric assembly (relative 1e-12), analytic single-column and uniform
multilayer solutions (1e-8 K), forced no-SciPy assembly/solve, hidden-DOF
recovery, energy (1e-9), Wiener bounds, multiple z cells, partition permutation,
malformed topology and unsupported outlines, optional gids with reordered/empty
partitions, and coverage/membership defects without gids. Mutation checks reject
duplicate or incorrect gids, missing fields/layers, wrong layer-name case, invalid
fractions, all-zero In1 copper, wrong conductivity and 1e-6 K temperature errors.
A scoped series-kz reference mutation preserves fractions and x/y coefficients,
then requires C4 to raise `M3 conductivity mismatch` and a validated fail record
whose only incomplete check is conductivity. The passing parallel control remains.
The four-layer B.Cu reference also checks nonempty zero/full-fraction selections
and the exact float64 endpoint representations of 0.3 and 390. Core checks and
given-coefficient Q2 self-checks retain their coefficients and tolerances; the
multilayer series resistance describes layers stacked through thickness.

## Verification (2026-11-21)

实测 main baseline: `c4e9eaeac95c7889cec1f9fa3c1ecc30fedb8d75`, containing
#200 (`8ad1211`) and the parallel-formula change #236 (`c4e9eae`).
Tested source: `3682d1a0d40deed8fc9de26d769de7acbd12f92b` (the regression-test
commit on that baseline); the subsequent change only updates this README.
Local `origin/main` was observed at `dc446e70a5151fafe9cec032509c90a4d36e30f4`
during this pass. It was not merged: the actual implementation baseline above
is retained. The reference already implements parallel conductivity in all
three directions, so no product or reference formula change was necessary.

The Release configuration enables tests, ECAD, JSONCONS, MFEM, PETSc, MPI and
HDF5. Installed MFEM/PETSc, MPI-enabled HDF5 and JSONCONS packages were used;
MPI launcher availability and NumPy/h5py imports were checked. PETSc discovery
uses the installed MFEM PETSc finder and its two helper modules in an external
module directory, avoiding MFEM's conflicting HDF5 finder. BLAS/LAPACK library
search paths were supplied through the environment. CMake emitted runtime
search-path warnings about competing `libgomp.so.1` locations; configuration,
build and the three backend tests succeeded. These are environment
adjustments, with no CMake or dependency source changes.

实测 targeted CTest: **3/3 passed, zero skips, 7.82 s**. The single e2e
entry point performs both ranks=1 and ranks=2 solve/readback attempts; no
second invocation with different flags is needed.

实测 direct unittest discovery: **30 tests, OK, zero skips**. The series-kz
mutation has maximum componentwise relative difference `0.99692780519449054`,
raises `M3 conductivity mismatch`, completes cell fractions and leaves only
conductivity incomplete in its validated `verdict: fail` record. This is a
caught negative assertion; the regression tests pass immediately on the
already-parallel implementation. No missing-feature RED is claimed.

实测 product measurements below are identical for ranks=1 and ranks=2. Both
`chipcae.vv_result/0.2` records validate and have `verdict: pass`; all nine
completion flags (`import`, `preview`, `solve_readback`, `inputs_unchanged`,
`cell_fractions`, `conductivity`, `physics`, `numerical`, `parallel`) are true.
C3 fractions/spatial variation/areas, finite C4 components and relative 1e-14,
C11 1e-8 K, energy 1e-9 and the stated Wiener interval assertions all execute.
Aligned fractions, conductivity, temperatures, displacement and QoIs pass the
existing MPI comparison at 1e-8. Shared inputs remain unchanged; preview and
both solves have 80 hexes and zero cut cells.

| Measurement (实测, both ranks) | Value |
| --- | --- |
| `max_temperature_error_K` | 3.4083313948940486e-10 K |
| `energy_relative_error` | 4.360084138426352e-12 |
| `Q_top_W` | 0.6630653593688077 W |
| `Q_bot_W` | 0.6630653593659167 W |
| `R_lo_K_W` | 66.9897035386662 K/W |
| `R_hi_K_W` | 76.13632975714098 K/W |
| Wiener heat-flow interval, before epsilon | [0.6567167101367977, 0.7463833598120073] W |
| F.Cu copper area | 0.307835 mm² |
| In1.Cu copper area | 2.25 mm² |
| In2.Cu copper area | 3.0000000000000004 mm² |
| B.Cu copper area | 6.999999999999999 mm² |
| Maximum copper-area relative error | 1.4802973661668753e-16 |
| `warpage_pv` / independent readback | 7.270602005916752e-06 m |
| `U_mag_max` | 7.1136040162876474e-06 m |
| `T_max` | 323.333553639902 K |
| Readback relative error | 0.0 |
| Parallel QoI relative errors (all three) | 0.0 |

实测 independent, non-gating Q2 diagnostics (K):

```text
trace_map_vs_layer_average_dT_max=5.0388880325680248
trace_map_nx_double_dT_max=0.59059340053721598
```

实测 full Release CTest: **exit 8; 484 registrations = 441 passed + 6 failed +
27 skipped + 10 disabled; 1845.54 s**. All three F-1130/chain tests passed
again. Skips and disabled tests do not establish acceptance. The all-suite
green requirement is unmet; no failing assertion or timeout was changed.

| Failed test | Observed failure |
| --- | --- |
| `solve_column_gmg_fcbga_scaling` | `scaling-fine-gmg` subprocess exit -9 after coarse solve completed; termination cause not established. |
| `solve_fem_thermal_bow` | C-0/A cross-run U residual 9.5759468232801005e-13 exceeds 2.5499999995493557e-15. |
| `cutbim_cut_bimetal` | B_nx16 ranks=1 solver exit -9; termination cause not established. |
| `field_compare_fem_fvm` | Solver status 14, `invalid output field`. |
| `field_compare_outline_bbox` | Solver status 14, `invalid output field`. |
| `field_compare_ranks` | Solver status 14, `invalid output field`. |

These failures are in unchanged files outside this card's two-file scope.
No separate baseline control run was performed, so no baseline-versus-branch
causal claim is made. A previous test-only pass recorded seven failures; its
`realcase_real_case_unit` failure did not recur in this fresh run (30.10 s,
Passed). The current status above comes from this execution.

Complete evidence accompanies the local PR draft: `v-int-f1130-z-parallel-ref-`
files with suffixes `final-configure.log`, `final-build.log`,
`final-unittest.log`, `final-targeted.log` (full commands and both rank outputs),
`final-ranks1-result.json`, `final-ranks2-result.json`, `final-all.log` and
`final-all.xml` (all failures and skipped/disabled names). The README contains
only current measurements; #200 old-versus-new differences are in the PR draft.

Remote `ci-ok`: **未执行** (no push); green `ci-ok` remains a merge requirement.
Missing-backend and missing-h5py skips do not establish acceptance. Dependency
failure-injection scenarios from #200 were not repeated in this pass: 未执行.
This gate checks implementation of the agreed trace-map model; explicit finely
resolved copper validation remains outside this gate.
