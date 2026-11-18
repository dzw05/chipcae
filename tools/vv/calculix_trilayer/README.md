<!-- SPDX-License-Identifier: Apache-2.0 -->

# WP-2 三层板 CalculiX 独立参考

Independent open-source reference for the 1 m × 1 m trilayer warpage case.
Results in `REFERENCE.json` are **REFERENCE values** (not measured data, not
an analytical solution). They may be compared with the early MFEM+MUMPS
prototype 参考值（原型计算值） `W_bfp = 4.269532769e-3` m. Validation applies only the broad `rel_delta < 0.5` (50%) sanity bound; the V&V
specialist chooses the comparison acceptance criterion later.

## Model

- Geometry and cards match the WP-2 trilayer: thicknesses 0.05 / 0.01 / 0.04 m,
  E = 25 / 3 / 130 GPa, ν = 0.2 / 0.35 / 0.28, α = 15e-6 / 50e-6 / 2.6e-6 1/K,
  ΔT = −100 K (400 K → 300 K).
- Thermal strain follows the [CalculiX manual, §7.56](https://www.dhondt.de/ccx_2.22.pdf#page=501):
  `alpha(T)*(T-Tref) - alpha(T0)*(T0-Tref)`. Each material explicitly sets
  `*EXPANSION, ZERO=400`; `*INITIAL CONDITIONS, TYPE=TEMPERATURE` sets all
  nodes to the stress-free 400 K state, and the step sets `*TEMPERATURE` to
  300 K. Thus the thermal strain is `alpha*(-100 K)` in every layer.
- Elements: C3D20 only (20-node quadratic bricks). Coarse 8×8×6, fine 16×16×6
  (two elements through each layer). The recorded value uses `selected_mesh = fine`.
- Boundary conditions: 3-2-1 on the z = 0 corners, as in
  `apps/chipcae-solve/README.md` (xmin/ymin fix ux,uy,uz; xmax/ymin fix uy,uz;
  xmin/ymax fix uz).
- Solver environment: `/workspace/tools/calculix-env`; conda-forge package `calculix=2.23`; the `ccx -v` runtime
  version string is `This is Version 2.23`.

## Regenerating

Requires numpy, scipy (used by the shared interpolation routine), and `ccx` on `PATH` (or `CHIPCAE_CCX`). From the repo root:

```
python tools/vv/calculix_trilayer/generate.py
```

The deterministic input decks [coarse.inp](coarse.inp) and [fine.inp](fine.inp)
are committed alongside the generator. The script regenerates these decks,
copies them into ignored `work/` for solving, and updates `REFERENCE.json`.
Solver outputs stay in `work/`; stale FRD results are deleted before each run.
A nonzero exit status or `*ERROR` in solver stdout/stderr fails generation.
Only nodes referenced by C3D20 elements are emitted, with compact IDs used by
NALL and the temperature cards. The coarse/fine meshes contain 2061/7565 nodes;
their TOP sets contain exactly 225/833 connected corner and edge-midpoint nodes.
The FRD reader requires the exact expected top-node IDs before interpolation,
and rejects every extra displacement record, including non-top or unknown IDs.
The ASCII `.frd` parser reads coordinates from the `2C`
block and displacements from the `-4 DISP` block, using fixed-width `-1`
node records until each `-3` terminator. Node IDs join coordinates and
displacements. Top-surface selection uses undeformed z at H = 0.10 m
within 1e-12 H, including C3D20 corner and midside nodes. Surface `W_bfp` is
computed with `tools.vv.warpage_qoi.interp_grid`
and `qoi_surface` on an 81×81 grid in millimetres, then converted back to metres.

The comparison test reads only `REFERENCE.json` and checks required normative
fields, model, refinement, and delta arithmetic. Schema version 0.1 requires
`schema_version` to be exactly `"chipcae.vv.reference/0.1"`. Required top-level
fields are `schema_version`, `kind`, `label`, `element_type`, `geometry`,
`meshes`, `selected_mesh`, `W_bfp_m`, `prototype_W_bfp_m`, `abs_delta_m`, and
`rel_delta`. The required `geometry` fields are `L_m`, `thicknesses_m`, `E_Pa`,
`nu`, `alpha_1_per_K`, and `dT_K`; each mesh requires `name`, `n_elem_xyz`,
`n_elem_z_per_layer`, and `W_bfp_m`. Optional runtime provenance
(`calculix_version` and `host`) does not affect validation; unknown extension
fields are allowed at the top level and in geometry and mesh objects.
`REFERENCE.json` is distributed under Apache-2.0. Source and deck headers also
record Apache-2.0.

The ASCII FRD layout used by CalculiX 2.23 is `(1X,I2,I10,3E12.5)`:
columns 2–3 contain the record marker, 4–13 the node ID, and 14–49
three 12-character real fields. Unsupported numeric layouts, duplicate IDs,
and incomplete coordinate/displacement blocks fail explicitly. Adjacent negative fields need no separating
space, so whitespace splitting is unsuitable. This same layout applies to
`2C` coordinates and `DISP` values. Run the parser regression checks with
`python tools/vv/calculix_trilayer/check_frd_parser.py`; these manual-only checks
are not collected by unittest discovery. They cover both blocks,
adjacent negative fields, exponent signs, node IDs, block termination, variable
header spacing, connected mesh nodes, and missing/extra displacements. Independent
deck checks verify the 3-2-1 corner mapping, material and temperature cards, C3D20
connectivity, and mesh counts against the JSON. The CI comparison remains JSON-only.

Recorded W_bfp: coarse 0.004271023422273522 m; fine (selected)
0.004274306997289974 m. Reproducible input decks are committed as `coarse.inp` and `fine.inp`;
rerun `generate.py` to recreate them. Scipy is used indirectly
by the shared `interp_grid` function for linear interpolation.

Run `python3 -m unittest tools.vv.test_trilayer_calculix_ref` after generation.
The comparison test uses only the standard library.
Only passing validation prints the single delta report in the brief's fixed
field order. The prototype is the 参考值（原型计算值） described above.
The passing comparison creates `/workspace/tasks/logs` if needed and overwrites
`/workspace/tasks/logs/v-ccx-delta.md` with exactly that single report line.
The manual parser regression compares this file with stdout, including when
`CHIPCAE_VV_LOG_DIR` is unset, empty, or set to another directory; the required
report path is fixed and does not depend on that variable.
