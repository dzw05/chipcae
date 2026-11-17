# WP-2 trilayer CalculiX reference

Independent open-source reference for the 1 m × 1 m trilayer warpage case.
Results in `REFERENCE.json` (`schema_version` 1) are **REFERENCE values** (not measured data, not
an analytical solution). They may be compared with the early MFEM+MUMPS
prototype `W_bfp = 4.269532769e-3` m. This directory does **not** set a
pass/fail gate; the V&V specialist chooses the comparison criterion later.

## Model

- Geometry and cards match the WP-2 trilayer: thicknesses 0.05 / 0.01 / 0.04 m,
  E = 25 / 3 / 130 GPa, ν = 0.2 / 0.35 / 0.28, α = 15e-6 / 50e-6 / 2.6e-6 1/K,
  ΔT = −100 K (400 K → 300 K).
- Elements: C3D20 only (20-node quadratic bricks). Coarse 8×8×6, fine 16×16×6
  (two elements through each layer). The recorded value uses `selected_mesh = fine`.
- Boundary conditions: 3-2-1 on the z = 0 corners, as in
  `apps/chipcae-solve/README.md` (xmin/ymin fix ux,uy,uz; xmax/ymin fix uy,uz;
  xmin/ymax fix uz).
- Solver: CalculiX 2.23 (`ccx`) from conda-forge, installed at
  `/workspace/tools/calculix-env`.

## Regenerating

Requires numpy, scipy, and `ccx` on `PATH` (or `CHIPCAE_CCX`). From the repo root:

```
python tools/vv/calculix_trilayer/generate.py
```

The script writes `work/{coarse,fine}.*` (not committed) and updates
`REFERENCE.json`. Surface `W_bfp` is computed with `tools.vv.warpage_qoi.interp_grid`
and `qoi_surface` on an 81×81 grid in millimetres, then converted back to metres.

The v1 field contract (required keys, types, and which fields are stable) is in `reference.schema.json` (JSON Schema 2020-12).

`provenance` is optional metadata. Ignoring unknown keys under it is a consumer convention, not a JSON Schema-enforced rule.
