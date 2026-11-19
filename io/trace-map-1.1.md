# I1 trace-map results contract 1.1

This version records the additive solver-output requirements approved in the
2026-11-19 I1 copper-base ruling 1.1, including its Q2 revision. It extends
the [O1 results contract](README.md#o1-results-contract); the manifest remains
`chipcae.results/1.0` and VTKHDF Version remains `[2,8]`. This document's version
identifies the trace-map requirements; it adds no on-disk version attribute.
Producer implementation belongs to `solve-copper-trace-map` (M3); the
F-1130 gate requires that implementation before product acceptance can pass.

## Fields and selection

The solver exports the coefficients actually used to assemble each cell.
For a block with N cells (summed over partitions), the datasets under its
`CellData` group have the following layouts:

| Name | dtype | Dataset shape | Manifest location / components | Physical unit |
| --- | --- | --- | --- | --- |
| `copper_fraction` | float64 | `(N,)` | cell / 1 | dimensionless |
| `conductivity_diag` | float64 | `(N,3)` | cell / 3 | W/(m K) |

Rows follow the block's cell/connectivity order and partition concatenation.
`ResultsPartition` stores fractions as N doubles and conductivity as 3N
interleaved doubles `(k_xx, k_yy, k_zz)`, in the global xyz axes. Empty blocks
retain typed shapes `(0,)` and `(0,3)` when these fields are selected. The same
per-cell rules apply to hex and cut volume blocks; these fields do not add
boundary or particle values. The existing manifest has no conductivity unit
spelling, so both descriptors use `unit: ""`; the physical units are defined
here. Descriptor `dtype` is `float64` for both fields.

M3 adds both names to the writer and solver default field lists. An explicit
`output.fields` list keeps the existing selection behaviour: only requested
fields are emitted. F-1130 requests both fields and treats either missing
field as a failure. Older O1 files and explicit selections omitting these
fields remain valid under O1; they cannot satisfy this trace-map gate.

## Cell semantics and F-1130 acceptance

For an uncut Patterned cell e, `copper_fraction` is the area fraction of copper
in that cell's xy column, `f_e = phi[layer][col(e)]`. It must be finite and in
`[0,1]`. The producer obtains the column from global mesh identity
`elem_gid / nz`, independently of rank-local cell ordering. This internal
mesh identity does not require exporting the optional O1 hex `elem_gid` field.
Non-Patterned cells export fraction zero.

With metal conductivity k_m and base conductivity k_b, the assembled diagonal is:

```text
k_xx = k_yy = f_e*k_m + (1-f_e)*k_b
k_zz = 1 / (f_e/k_m + (1-f_e)/k_b)
```

The diagonal is finite and positive. At f_e=0 it is the base conductivity,
and at f_e=1 it is the metal conductivity. Non-Patterned cells export their
own material diagonal. A missing phi or a cut block never justifies exporting
a zero conductivity. Cut/merged-cell fractions follow the separate I1 1.1
solid-area and merge rules; F-1130 covers uncut cells only.

F-1130 independently clips the copper polygons to each column. Its C3 check
requires absolute fraction error at most `1e-12`, spatial variation within
each copper layer, and relative copper-area error at most `1e-12`. Area is
`sum(f_e*V_e)/t_layer`, avoiding repeated counting across z cells. C4 compares
each conductivity component with relative error at most `1e-14`, using fixed
fixture k_m=390 and k_b=0.3 W/(m K). Core has fraction zero and diagonal
`(0.3,0.3,0.3)`. These are acceptance tolerances, not dataset encoding rules.

Hex `elem_gid` remains optional int64 with its existing dense Morton column
times global nz plus k meaning. When present, F-1130 checks unique IDs,
complete coverage and their agreement with physical cell bounds. When absent,
it aligns cells by exact rational bounds and checks the same complete, unique
uncut tensor-grid coverage and layer/z membership. Cell and vertex fields
remain compared independently of partition ordering. Dense coverage is a
requirement of this rectangular uncut fixture; general cut outputs may have
gaps in their IDs.

## Recorded owner approvals

These are existing approvals of I1 1.1, transcribed from the project handoff
records. This documentation change does not create a new approval or freeze
the rest of I6/O1. The ruling requires mesh, solver and vv signatures, with
desktop notification.

| Owner | Recorded approval (2026-11-19) | Evidence identifier and conditions |
| --- | --- | --- |
| mesh (M2) | 13:10, I1 1.1 trace mapping signed | `handoff/status/mesh.md`: use global elem_gid for columns; polygon area means union area. |
| solver (M3, producer) | 13:36, I1 1.1 trace mapping signed | `handoff/status/solver.md`: Q2 reference, 3x3x3/3x3 Gauss, vertex comparison; coarse GMG uses Galerkin. Supersedes the earlier Q1 objection. |
| vv (INT, consumer) | 13:20, I1 1.1 trace mapping signed | `handoff/status/vv.md` and `handoff/rulings/i1-copper-base.md`: fraction tolerance is absolute 1e-12; area uses volume/thickness for multiple z cells. |
| desktop (notification) | 09:55, new output fields announced | `handoff/inbox/desktop.md`: additive copper_fraction and conductivity_diag fields. No signature required by the ruling. |

The concrete layouts and selection rules are recorded in the approved work
brief `solve-copper-trace-map`, under its public-signature section. Changes to
these semantics require a new contract revision and the ruling's owner review
before merge.
