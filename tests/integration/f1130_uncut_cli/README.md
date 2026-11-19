# F-1130 trace-map CLI acceptance

This gate checks I1 1.1 C3/C4/C11 on the unchanged public synthetic
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

## I1 1.1 consumer expectations

This gate consumes the per-cell coefficients specified by the I1 copper-base
ruling 1.1 (2026-11-19, including its Q2 revision) and the producer brief
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
approve the model used by this consumer; they do not establish a new public
I/O contract in this card.

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
The [I1 1.1 consumer expectations](#i1-11-consumer-expectations) above describe
the two required coefficient fields and the recorded owner approvals.
Cells are aligned by globally unique `elem_gid` when exported, with complete
dense Morton column coverage and layer/z membership checked independently.
O1 permits omitting hex `elem_gid`; in that case exact rational cell bounds
provide alignment, with complete, unique coverage and layer/z checks retained.
Dense coverage belongs to this uncut fixture. Vertices are
aligned by rational coordinates; repeated boundary temperatures/displacements
must agree before deduplication. Partition ordering cannot choose the column.

For each copper cell, with independently calculated f:

```text
k_x = k_y = f*390 + (1-f)*0.3
k_z = 1 / (f/390 + (1-f)/0.3)
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
  -DCHIPCAE_ENABLE_MPI=ON -DCHIPCAE_ENABLE_HDF5=ON
cmake --build "$build" -j2
ctest --test-dir "$build" -V \
  -R '^(f1130_uncut_cli_records|f1130_uncut_cli_e2e|chain_ipc_uncut_chain)$'
ctest --test-dir "$build" --output-on-failure -j2
python3 -m unittest discover -s tests/integration/f1130_uncut_cli \
  -p test_uncut_cli_e2e.py -v
```

CMake supplies executable locations when ECAD, MFEM, MPI and HDF5 are enabled.
Missing executable configuration or NumPy/h5py returns the existing skip code
77; configured command failures fail acceptance. Commands have 600 second
timeouts. A missing MPI launcher fails, and no retired fixture gate suppresses
configured acceptance.

The records discovery path includes 13 assertion/verdict tests and
16 reference tests: exact clipping, signed holes and empty copper, binary
validation and exact fixture areas, Q2 partition/derivative sums (1e-14),
symmetric assembly (relative 1e-12), analytic single-column and uniform
multilayer solutions (1e-8 K), forced no-SciPy assembly/solve, hidden-DOF
recovery, energy (1e-9), Wiener bounds, multiple z cells, partition permutation,
malformed topology and unsupported outlines, optional gids with reordered/empty
partitions, and coverage/membership defects without gids. Mutation checks reject
duplicate or incorrect gids, missing fields/layers, wrong layer-name case, invalid
fractions, all-zero In1 copper, wrong conductivity and 1e-6 K temperature errors.

## Verification (2026-11-20)

实测 source: the merged working tree based on fetched main
`c9c1464bad72444d98bf52bb4d0848d530095281`, including M1 `92a21a4` (#195)
and M3 `bcc40b3` (#212). Main was merged without rebasing. The PR diff against
that baseline remains restricted to this card's four permitted files.

实测 Release configure and build exit 0 with tests, ECAD, JSONCONS, MFEM, MPI
and HDF5 enabled, optional Kokkos disabled, and Python 3.12 selected. CMake
reported dependency runtime-search-path warnings; compilation succeeded.
实测 direct unittest discovery: **29 tests, OK** (13 assertion/verdict tests
and 16 reference tests), including SciPy sparse and forced pure NumPy paths.
The solver-input assertion and diagnostic-exception regression failed before
the fixes and passed afterward. The latter verifies that preview, both solves,
readback and aligned comparison remain available after a diagnostic exception;
both records retain the diagnostic failure. The matrix regression also checks
literal half-copper FR4 coefficients and rejects an incorrect copper base.

实测 targeted CTest: exit **0**, **3/3 Passed**, **8.58 s**:
`f1130_uncut_cli_records`, `f1130_uncut_cli_e2e`, and
`chain_ipc_uncut_chain`. Four-layer preview and solve each produced 80 uncut
hexes. Both ranks completed solve/readback, preserved all shared input bytes,
and produced validated records with `verdict: pass`; import, fractions,
conductivity, physics, numerical and parallel completion checks were all true.
Aligned cell/vertex fields passed at the unchanged `PARALLEL_RTOL=1e-8`;
all three QoI relative differences were zero.

实测 product measurements (identical for ranks=1 and ranks=2):

| Measurement | Value |
| --- | --- |
| `max_temperature_error_K` | 5.348965714802034e-11 K |
| `energy_relative_error` | 5.588270495424436e-15 |
| `Q_top_W` | 0.6357447589033838 W |
| `Q_bot_W` | 0.6357447589033802 W |
| `R_lo_K_W` | 75.31414010424379 K/W |
| `R_hi_K_W` | 78.93330782246156 K/W |
| Wiener heat-flow interval | [0.6334461506726798, 0.6638859572823113] W |
| F.Cu copper area | 0.307835 mm² |
| In1.Cu copper area | 2.25 mm² |
| In2.Cu copper area | 3.0000000000000004 mm² |
| B.Cu copper area | 6.999999999999999 mm² |

The maximum area relative error was 1.480297366166875e-16. C11 remains
1e-8 K; C3/C4, area, energy and parallel acceptance thresholds are unchanged.
The permitted `rtol=1e-12` change affects the solver input only.

实测 independent, non-gating Q2 diagnostics (K):

```text
trace_map_vs_layer_average_dT_max=1.4087819663407117
trace_map_nx_double_dT_max=0.20071960596669669
```

实测 two-layer chain: 24 uncut cells, `warpage_pv=4.385282668532796e-6` m,
`T_max=340.53308548486064` K; import, per-cell reference checks and readback
passed without calling the layer-average conversion tool.

实测 full Release CTest: exit **8**, **416 total = 381 passed +
11 failed + 14 skipped + 10 already disabled**, **11187.08 s**. All three
requested F-1130/chain tests passed again in this full run. The 10 disabled
registrations and feature-dependent skips come unchanged from main; this
fix adds none. All failures are listed individually:

| Failed test | Reported elapsed (s) | Observed failure |
| --- | --- | --- |
| `ipc_zip_equals_xml` | 0.04 | Unchanged ZIP test supplies obsolete dotted material IDs, rejected by current ID validation. |
| `k1_preview_mpi_surface` | 2.00 | Non-JSON run_config fragment in captured stdout; jsoncons parsing aborted. |
| `solve_cut_pipeline` | 826.00 | Two-rank fcbga-polyset subprocess exceeded its existing 240 s timeout. |
| `solve_ghost_scan` | 1470.81 | Existing 300 s CTest timeout; reported elapsed 1470.81 s. |
| `solve_cut_bilayer` | 1723.27 | Existing 1200 s CTest timeout during the 16×16 case. |
| `solve_column_gmg_fcbga` | 1404.16 | Existing 900 s CTest timeout after fine-grid comparisons were printed. |
| `solve_column_gmg_fcbga_scaling` | 1715.65 | Existing 900 s CTest timeout after coarse-GMG output. |
| `cutbim_cut_bimetal` | 1497.73 | 16×16 ranks=1 solver subprocess exceeded 420 s; comparison quantity missing. |
| `demogate_solve` | 1013.92 | Zero-load solver subprocess exceeded its existing 420 s timeout. |
| `stack_writer_outline_polyset` | 1.63 | Solver exit 11: duplicate material id from the unchanged assembly helper. |
| `import_assembly_cf_chain` | 1.71 | Solver exit 11: duplicate material id from the unchanged assembly helper. |

The listed tests, their helpers and product code match the merged main
baseline. No out-of-scope product/helper change was made. Several reported
elapsed times exceed the configured limits, and tool responses also suffered
abnormal wall delays. The measurements are retained as reported without
attributing a cause or adjusting timeouts. The seven long timeout cases were
not rerun. Full-suite all-green status was **not achieved**.

实测 unchanged isolated follow-up: `k1_preview_mpi_surface` passed, exit **0**, **1/1**, **44.26 s**, using `ctest -j1` and its original assertions/timeouts. This does not replace the original full-suite failure or counts. The full suite was not rerun.

The mesh/solver/vv I1 1.1 signatures are complete according to the PR #200
architect ruling; no PR hold remains required. CI for the unpushed fix commit
is 未执行. Merge still requires green ci-ok. This gate checks implementation
of the agreed trace-map model; explicit finely resolved copper validation
remains outside this gate.
