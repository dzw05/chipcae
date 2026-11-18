# Synthetic ODB++ / IPC-2581 CLI acceptance

`odbcli_odbpp_cli_e2e` is a test-first acceptance driver, labelled
`integration;numerical;vv`, with a 1200 second CTest timeout. All input is public
synthetic data. The illustrative material cards are not calibrated board data.

```text
synthetic kicad4l → temporary reduced ODB++ and IPC-2581
→ chipcae-import (each format; IPC accepts B-tier defects)
→ copper_fraction.py (each format)
→ compare layer sequence, materials, thickness, decoded area and fractions
→ import changed dielectric / import and average swapped inner copper (negative comparisons)
→ ODB averaged stack → mesh.stack.json → chipcae-preview
→ same case / stack / materials → chipcae-solve ranks=1, ranks=2 if launcher exists
→ VTKHDF temperature versus 1D resistance, cell counts and manifest QoIs
→ result_summary.py → per-run result.json 0.2 → check_file
```

The driver copies `tests/integration/ecad/data/odbpp/kicad4l` into a fresh temporary
directory. Matrix entries are filtered with the existing `Board` test's
`LAYER\s*\{[^}]*\}` rule, retaining only SIGNAL and DIELECTRIC. Copper features
use `UNITS=MM`, one `rect<W>x<H>` symbol with dimensions in micrometres, and
`P 30 -25 0 P 0 0`. Rectangle dimensions in stack order are 10×10, 20×15, 30×20
and 36×25 mm. Dielectric features contain only `UNITS=MM`. The profile is retained:
x=[10,50], y=[−40,−10] mm, area 1200 mm². Derived fixtures are never committed.

IPC XML is transformed with namespace-aware ElementTree tags. Every LayerFeature,
Component, Package and PadStackDef is removed. Layer definitions, Stackup,
material specifications and Profile remain. Four LayerFeature/Set/Polygon solid
rectangles are inserted using the existing `board_4layer.xml` structure, with the
same centres and dimensions as ODB. An initial real CLI run returned exit 7:
C-tier defects identified two Oval entries and two UserSpecial/Circle entries.
The now-unused DictionaryStandard and DictionaryUser containers are also deleted
(their referencing LayerFeature/Package/PadStackDef elements are already gone).
These are symbol definitions, not board structure; the profile and stack are retained.
The negative IPC swaps only the In1.Cu / In2.Cu dimensions; the negative ODB
changes only DIELECTRIC_2 `.layer_dielectric` to 1.07 mm.

ASCII case alone is folded for comparisons; duplicate folded names and missing
layers fail. Geometry expectations are hand derived: fractions 1/12, 1/4, 1/2,
3/4; F.CU area 1e-4 m²; outline 1.2e-3 m². `tools.stack.polyset` decodes the copper
sidecars. The copper thickness exception is pinned to 30 µm ODB and 35 µm IPC,
with relative tolerance 1e-12, and printed as “已知偏差，待裁定” (M1). Dielectrics
must agree to 1e-6 relative. No absolute slack is added to relative comparisons.

The original imported and averaged stacks are preserved. Only `mesh.stack.json`
sets `n_elem_min=ceil(thickness/min_thickness)`. Preview and solves use nx=ny=4,
levels=0, max_ratio=100. The same case, stack and material bytes feed both solves;
profiles differ only in ranks. The first available mpiexec, then mpirun, is used.
A configured executable or launcher failure fails the test. Exit 77 is reserved
for absent executable environment variables or absent numpy/h5py.

Analytical temperature uses the actual layer material and copper metal IDs,
conductivities read from the illustrative cards, and fractions from cf.json.
Each layer uses `1/(f/k_metal+(1-f)/k_layer)` when copper is present. With h=1000,
bottom temperature 298.15 K and ambient 348.15 K, heat flux is
`50/(sum(t/k)+1/h)` and temperature follows cumulative 1D resistance.
Acceptance is maximum absolute error ≤1e-8 K. If both IDs are mat.cu, this formula
has no dependence on copper fraction; the geometry assertions remain independent.

## Validation and measurements

Measured (实测) on 2026-11-19, baseline commit `b703d573f332b7716487deceea0cbe603383ba0f`,
Release, ECAD + JSONcons + MPI + parallel HDF5 + PETSc-enabled MFEM. The three
CLI targets built successfully. Empty dielectric features were accepted; no
fallback features were necessary.

| Layer (ASCII folded) | ODB thickness m (实测) | IPC thickness m (实测) | IPC − ODB m |
|---|---:|---:|---:|
| f.cu | 0.000030 | 0.000035000000000000004 | 0.000005 |
| dielectric_1 | 0.0002 | 0.0002 | 0 |
| in1.cu | 0.000030 | 0.000035000000000000004 | 0.000005 |
| dielectric_2 | 0.00106 | 0.0010600000000000002 | ≈2.17e-19 |
| in2.cu | 0.000030 | 0.000035000000000000004 | 0.000005 |
| dielectric_3 | 0.0002 | 0.0002 | 0 |
| b.cu | 0.000030 | 0.000035000000000000004 | 0.000005 |

Copper differences are the pinned “已知偏差，待裁定” (M1), not a relaxed tolerance.

| Copper layer | ODB fraction (实测) | IPC fraction (实测) |
|---|---:|---:|
| f.cu | 0.08333333333333331 | 0.08333333333333331 |
| in1.cu | 0.24999999999999994 | 0.24999999999999994 |
| in2.cu | 0.4999999999999999 | 0.4999999999999999 |
| b.cu | 0.7499999999999999 | 0.7499999999999999 |

Both reports measure outline area `0.0012000000000000001` m² and F.CU copper area
`9.999999999999999e-05` m². Independent polyset decoding passes the F.CU area gate.
Both formats map each copper layer's `material` and `copper.metal` to `mat.cu`;
all three dielectric materials are `mat.fr4_eq`. Normal thickness comparison has
no unexpected names and exactly the four copper names as known deviations.
The negative thickness comparison reports only `dielectric_2`; the swapped
fraction comparison reports only `in1.cu`, `in2.cu`.

The mesh counts are `[1,7,1,36,1,7,1]` in layer order. Preview measures 864 cells.
`mpiexec` is available (first-choice launcher). Serial solve is blocked before
numerical execution, so ranks=2, temperatures, warpage, result_summary and the
result.json gate have **not** been measured. Acceptance assertions for those
stages are present and remain enabled.

Use a backend-enabled Release build, referenced here as `$build`. Configure with
`CHIPCAE_ENABLE_ECAD`, `CHIPCAE_ENABLE_JSONCONS`, `CHIPCAE_ENABLE_MFEM`,
`CHIPCAE_ENABLE_MPI`, `CHIPCAE_ENABLE_HDF5`, and MFEM built with PETSc/direct
solver support. Build and test commands (plain wrapped tool names):

```sh
cmake --build "$build" --target chipcae-import chipcae-preview chipcae-solve -j2
ctest --test-dir "$build" -R '^odbcli_odbpp_cli_e2e$' --output-on-failure
```

| Local check | Command | Exit | Maximum temperature error |
|---|---|---:|---|
| Normal | CTest command above | 8 | unavailable: solve exit 14 |
| Reference heat flux temporarily ×1.01 | same CTest command | 8 | unavailable: solve exit 14 |
| Reference restored byte for byte | same CTest command | 8 | unavailable: solve exit 14 |

The mutation was removed and is not part of the committed driver. It cannot yet
prove the temperature assertion: all three runs stop at the same earlier product
contract conflict. The exact diagnostic is:

```text
chipcae-solve: material mat.cu /id: String 'mat.cu' does not match pattern '^[a-z0-9_-]+$'.
```

`material/schema/material.v1.schema.json` rejects the dotted IDs mandated by this
brief and used by the existing chain test. This is a runtime material-contract
failure (M3/material), after successful imports, averaging, comparisons and
preview, not a compilation error or missing backend. No product files or material
IDs were changed to hide it. No production stubs were needed.

Dependency checks measure exit 77 for each missing executable variable and for
missing numpy. Configured nonexistent executables fail with exit 1. A backend-off
CTest configuration is checked separately for the expected skip. Full Release build also exits 0. Full-suite CTest exits 8: 249 passed, 7 failed,
5 skipped (261 total). Failures are the new acceptance, plus existing event_log,
odbpp_map_placeholder (60 s timeout), ipc_fixture_present, solve_inputs,
chain_ipc_uncut_chain and ecaddiff_import_pair. event_log passes on an isolated
TMPDIR repeat; its original run encountered shared temporary salvage files.
Exact machine-local commands/output and failure details are recorded in the
external TDD task log. This test-first delivery keeps the acceptance red.

## Limits

This test excludes whole-board slots/holes, `--stack-override` thickness overrides,
cut geometry, and real boards. It does not change the whole-board rejection
expectations or importer policy. Backend-enabled self-hosted/nightly builds run
the numerical chain; hosted builds without the gated dependencies skip it.
