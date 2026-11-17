# WP-2 trilayer CalculiX reference

Independent open-source reference for the 1 m × 1 m trilayer warpage case.
Results in `REFERENCE.json` (`schema_version` 1) are **REFERENCE values** (not measured data, not
an analytical solution). They may be compared with the early MFEM+MUMPS
prototype `W_bfp = 4.269532769e-3` m. Validation applies only the broad `rel_delta < 0.5` sanity bound; the V&V
specialist chooses the comparison acceptance criterion later.

## Model

- Geometry and cards match the WP-2 trilayer: thicknesses 0.05 / 0.01 / 0.04 m,
  E = 25 / 3 / 130 GPa, ν = 0.2 / 0.35 / 0.28, α = 15e-6 / 50e-6 / 2.6e-6 1/K,
  ΔT = −100 K (400 K → 300 K).
- Elements: C3D20 only (20-node quadratic bricks). Coarse 8×8×6, fine 16×16×6
  (two elements through each layer). The recorded value uses `selected_mesh = fine`.
- Boundary conditions: 3-2-1 on the z = 0 corners, as in
  `apps/chipcae-solve/README.md` (xmin/ymin fix ux,uy,uz; xmax/ymin fix uy,uz;
  xmin/ymax fix uz).
- Solver: conda-forge package `calculix=2.23`; the `ccx -v` runtime
  version string is `This is Version 2.23`.

## Regenerating

Requires numpy, scipy, and `ccx` on `PATH` (or `CHIPCAE_CCX`). From the repo root:

```
python tools/vv/calculix_trilayer/generate.py
```

The script writes `work/{coarse,fine}.*` (not committed) and updates
`REFERENCE.json`. The ASCII `.frd` parser reads coordinates from the `2C`
block and displacements from the `-4 DISP` block, using fixed-width `-1`
node records until each `-3` terminator. Node IDs join coordinates and
displacements. Top-surface selection uses undeformed z at H = 0.10 m
within 1e-12 H, including C3D20 corner and midside nodes. Surface `W_bfp` is
computed with `tools.vv.warpage_qoi.interp_grid`
and `qoi_surface` on an 81×81 grid in millimetres, then converted back to metres.

The v1 field contract (required keys, types, and which fields are stable) is in `reference.schema.json` (JSON Schema 2020-12).

`provenance` is optional metadata and permits unknown keys within that object;
unknown top-level keys are rejected.

The v1 contract requires exactly one coarse and one fine mesh and selects fine.
Top-level `W_bfp_m` must equal the selected mesh value; validation checks this
and the broad `rel_delta < 0.5` sanity bound. Required `label` and `host` fields
record the reference status and execution host.

Run `python3 -m unittest tools.vv.test_trilayer_calculix_ref` after generation.
Only passing validation prints the delta report and checks the same single line
in a temporary report file, which is removed when the test completes. External
callers can capture stdout to retain the report; generation does not write it.
