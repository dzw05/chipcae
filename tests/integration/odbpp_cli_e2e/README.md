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
levels=0, max_ratio=100. The case explicitly sets `min_elems_per_layer=1` so
the solver preserves these counts instead of applying its default minimum of two.
The same case, stack and material bytes feed both solves;
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

Measured (实测) on 2026-11-19, review fix based on commit `236e4400b47fcc533f5bfe89106009dde3f840c0`,
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

The mesh counts are `[1,7,1,36,1,7,1]` in layer order. Preview and both
solves measure 864 regular cells and zero cut cells. `mpiexec` is available
(first-choice launcher). Both ranks=1 and ranks=2 complete solve, VTKHDF checks,
`result_summary.py`, and the per-run `result.json` 0.2 `check_file` gate.

| Quantity (实测) | ranks=1 | ranks=2 |
|---|---:|---:|
| Maximum absolute temperature error K | 9.366658559883945e-10 | 9.366658559883945e-10 |
| Manifest warpage PV m | 0.00011369859140462488 | 0.00011369859140462488 |
| Maximum displacement magnitude m | 0.00011400375798343959 | 0.00011400375798343959 |
| Maximum temperature K | 339.6277197011525 | 339.6277197011525 |

The material schema now accepts dots alongside the existing lowercase letters,
digits, underscores and hyphens. A schema regression validates `mat.cu` and
`mat.fr4_eq` through the material-library reference and retains invalid-ID checks.
The earlier acceptance and implementation runs stopped at solve exit 14 because
this schema excluded dots; those runs did not measure numerical results.

Running beyond that blocker exposed an internal interface attribute aliasing
the top convection face. The solver now spaces per-layer boundary IDs using the
full boundary attribute range, including aligned interfaces. Before this fix the
unchanged temperature gate measured a 3.8645886088048087 K error; after it, both
rank counts pass the original 1e-8 K gate. No material IDs, imported layer data,
conductivities, load values or numerical tolerances were changed.

Readback distinguishes the manifest's Q2 top-node warpage from the exported
vertex best-fit-plane warpage. These use different sample sets. The driver checks
manifest preservation and independently fits the exported displacement vertices,
comparing each summary field with its matching reference at 1e-12 relative
tolerance. It also requires a finite, positive exported-vertex warpage.

Use a backend-enabled Release build, referenced here as `$build`. Configure with
`CHIPCAE_ENABLE_ECAD`, `CHIPCAE_ENABLE_JSONCONS`, `CHIPCAE_ENABLE_MFEM`,
`CHIPCAE_ENABLE_MPI`, `CHIPCAE_ENABLE_HDF5`, and MFEM built with PETSc/direct
solver support. Build and test commands (plain wrapped tool names):

```sh
cmake -S . -B "$build" -DCMAKE_BUILD_TYPE=Release
cmake --build "$build" -j2
OMP_NUM_THREADS=1 ctest --test-dir "$build" -R '^(case_run_schema_fixtures|odbcli_odbpp_cli_e2e)$' --output-on-failure -V
OMP_NUM_THREADS=1 ctest --test-dir "$build" --output-on-failure -j2
```

| Local check | CTest exit | Maximum temperature error K | Reached stage |
|---|---:|---:|---|
| Normal | 0 | 9.366658559883945e-10 | ranks=1/2 solve, readback, result.json |
| Reference heat flux temporarily ×1.01 | 8 | 0.41477719793664392 | ranks=1 temperature assertion after solve exit 0 |
| Reference restored byte for byte | 0 | 9.366658559883945e-10 | ranks=1/2 solve, readback, result.json |

Only the analytical reference flux was multiplied by 1.01; the case and solver
inputs were unchanged. The mutation is absent from the final driver. Restoration
was checked against the saved driver bytes before rerunning the same CTest.
The normal targeted run passes 2/2 tests; the restored E2E run passes 1/1.
Release configure and the full build exit 0. The touched C++ file was formatted
with clang-format 23.1.5 using `-i`, followed by a clean `--dry-run --Werror`.
Full Release CTest exits 8: 252 passed, 4 failed, 5 skipped (261 total;
1174.62 s). The new acceptance passes within this suite. The four failures also
appear in the pre-fix implementation record; this scoped review fix does not
claim an all-green repository suite:

| Failed test | Observed failure |
|---|---|
| `odbpp_map_placeholder` | 60 s timeout in the large-contour performance case |
| `ipc_fixture_present` | eight bare geometry-context classification assertions |
| `chain_ipc_uncut_chain` | expects copper-layer material `mat.fr4`; importer returns `mat.cu` |
| `ecaddiff_import_pair` | expects hard/discuss/info=11/0/5; receives 0/0/14 |

`event_log`, `vtkhdf_results_mpi_2`, and `solve_inputs` pass. The five skips are
the two GPU tests and three optional assembly/STEP placeholders. No tests were
excluded and no test timeout was increased.

Product handoffs: [M3/material #168](https://github.com/dzw05/chipcae/issues/168)
records the earlier dotted-ID blocker, addressed by this fix;
[M1 #169](https://github.com/dzw05/chipcae/issues/169) tracks the pinned copper
thickness discrepancy. The latter preserves the prior
[#82](https://github.com/dzw05/chipcae/issues/82) ruling that `copper_weight` is
not a thickness source. Importer policy and the known-deviation gates are unchanged.

## Limits

This test excludes whole-board slots/holes, `--stack-override` thickness overrides,
cut geometry, and real boards. It does not change the whole-board rejection
expectations or importer policy. Backend-enabled self-hosted/nightly builds run
the numerical chain; hosted builds without the gated dependencies skip it.
