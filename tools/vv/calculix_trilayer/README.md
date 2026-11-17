# WP-2 trilayer CalculiX reference

Independent open-source reference for the 1 m × 1 m trilayer warpage case.
Results in `REFERENCE.json` are **REFERENCE values** (not measured data, not
an analytical solution). They may be compared with the early MFEM+MUMPS
prototype 参考值（原型计算值） `W_bfp = 4.269532769e-3` m. Validation applies only the broad `rel_delta < 0.5` sanity bound; the V&V
specialist chooses the comparison acceptance criterion later.

## Model

- Geometry and cards match the WP-2 trilayer: thicknesses 0.05 / 0.01 / 0.04 m,
  E = 25 / 3 / 130 GPa, ν = 0.2 / 0.35 / 0.28, α = 15e-6 / 50e-6 / 2.6e-6 1/K,
  ΔT = −100 K (400 K → 300 K).
- Thermal strain follows the [CalculiX manual, §7.56](https://www.dhondt.de/ccx_2.22.pdf#page=501):
  `alpha(T)*(T-Tref) - alpha(T0)*(T0-Tref)`. With constant alpha in each
  material, the reference temperature cancels, so the initial 400 K and target
  300 K cards give exactly `alpha*(-100 K)` without an explicit `ZERO`.
- Elements: C3D20 only (20-node quadratic bricks). Coarse 8×8×6, fine 16×16×6
  (two elements through each layer). The recorded value uses `selected_mesh = fine`.
- Boundary conditions: 3-2-1 on the z = 0 corners, as in
  `apps/chipcae-solve/README.md` (xmin/ymin fix ux,uy,uz; xmax/ymin fix uy,uz;
  xmin/ymax fix uz).
- Solver environment: `/workspace/tools/calculix-env`; conda-forge package `calculix=2.23`; the `ccx -v` runtime
  version string is `This is Version 2.23`.

## Regenerating

Requires numpy, scipy (used by the shared interpolation routine), jsonschema, and `ccx` on `PATH` (or `CHIPCAE_CCX`). From the repo root:

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

The shared `contract.py` module defines the artifact schema and validator.
The required integer `artifact_version` is `1`; missing or unsupported versions
are rejected by both producer and consumer.
Both `generate.py` (before writing `REFERENCE.json`) and
`tools/vv/test_trilayer_calculix_ref.py` use this contract. It checks required fields,
coarse/fine mesh names, fine selection, and equality with the fine mesh value.
Optional `provenance` metadata cannot replace required top-level environment strings.

The ASCII FRD layout used by CalculiX 2.23 is `(1X,I2,I10,3E12.5)`:
columns 2–3 contain the record marker, 4–13 the node ID, and 14–49
three 12-character real fields. Adjacent negative fields need no separating
space, so whitespace splitting is unsuitable. This same layout applies to
`2C` coordinates and `DISP` values. Run the parser regression checks with
`python tools/vv/calculix_trilayer/check_frd_parser.py`; these manual-only checks
are not collected by unittest discovery. They cover both blocks,
adjacent negative fields, exponent signs, node IDs, and block termination.

Recorded W_bfp: coarse 0.004271023422273522 m; fine (selected)
0.004274306997289974 m. Reproducible input decks are generated in `work/`
by `generate.py`; rerun the generator to recreate them. Scipy is used indirectly
by the shared `interp_grid` function for linear interpolation.

Run `python3 -m unittest tools.vv.test_trilayer_calculix_ref` after generation.
The validator requires `jsonschema`, which the lint workflow installs.
Only passing validation prints the delta report and checks the same single line
in a temporary report file, which is removed when the test completes. External
callers can capture stdout to retain the report; generation does not write it.
