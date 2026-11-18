# Synthetic ODB++ / IPC-2581 CLI acceptance

The fixture uses schema-valid material IDs `cu` and `fr4_eq`, consistently in
mapping values, material cards and assertions. The illustrative card values are
unchanged: copper E=110e9 Pa, ν=0.34, α=17e-6 K⁻¹, k=390 W/(m·K);
dielectric E=25e9 Pa, ν=0.3, α=1e-5 K⁻¹, k=0.3 W/(m·K).
The frozen material schema is unchanged.

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
Acceptance is maximum absolute error ≤1e-8 K. If both IDs are cu, this formula
has no dependence on copper fraction; the geometry assertions remain independent.

## Solver regression coverage

The E2E's layered 1D temperature check covers boundary-attribute aliasing in
`apps/chipcae-solve/src/pipeline.cpp`. Aligned interfaces receive original
boundary attributes above 6. A per-layer stride of 6 can give an interface the
same remapped ID as an exterior face, applying the top convection load to that
interface too. The solver must space per-layer IDs using the maximum original
boundary attribute. Each MPI rank constructs the same global serial `mfem::Mesh`,
so this stride has the same value on both ranks.

## Measured acceptance (实测)

Measured on 2026-11-19 in a backend-enabled Release build (ECAD, JSONcons, MPI,
parallel HDF5 and MFEM with PETSc/direct solver support). Empty dielectric
features were accepted; no fallback features were needed.

| Layer (ASCII folded) | ODB thickness m (实测) | IPC thickness m (实测) | IPC − ODB m |
|---|---:|---:|---:|
| f.cu | 0.000030 | 0.000035000000000000004 | 0.000005 |
| dielectric_1 | 0.0002 | 0.0002 | 0 |
| in1.cu | 0.000030 | 0.000035000000000000004 | 0.000005 |
| dielectric_2 | 0.00106 | 0.0010600000000000002 | ≈2.17e-19 |
| in2.cu | 0.000030 | 0.000035000000000000004 | 0.000005 |
| dielectric_3 | 0.0002 | 0.0002 | 0 |
| b.cu | 0.000030 | 0.000035000000000000004 | 0.000005 |

| Copper layer | ODB fraction (实测) | IPC fraction (实测) |
|---|---:|---:|
| f.cu | 0.08333333333333331 | 0.08333333333333331 |
| in1.cu | 0.24999999999999994 | 0.24999999999999994 |
| in2.cu | 0.4999999999999999 | 0.4999999999999999 |
| b.cu | 0.7499999999999999 | 0.7499999999999999 |

Both reports measure outline area `0.0012000000000000001` m² and F.CU copper area
`9.999999999999999e-05` m²; independent polyset decoding passes the area gate.
Both formats map each copper layer's `material` and `copper.metal` to `cu`;
all dielectric materials are `fr4_eq`. The normal comparison reports no
unexpected thickness differences and exactly four known copper deviations.
Negative comparisons report only `dielectric_2` for thickness and only
`in1.cu`, `in2.cu` for swapped fractions.

The per-layer counts are `[1,7,1,36,1,7,1]`. Preview and both solves measure
864 regular cells and zero cut cells. The first-choice `mpiexec` launcher is
available. Both ranks complete solve, VTKHDF checks, `result_summary.py` and
per-run `result.json` 0.2 validation.

| Quantity (实测) | ranks=1 | ranks=2 |
|---|---:|---:|
| Maximum absolute temperature error K | 9.366658559883945e-10 | 9.366658559883945e-10 |
| Manifest warpage PV m | 0.00011369859140462488 | 0.00011369859140462488 |
| Maximum displacement magnitude m | 0.00011400375798343959 | 0.00011400375798343959 |
| Maximum temperature K | 339.6277197011525 | 339.6277197011525 |

Readback checks preserve the manifest's Q2 top-node warpage and independently
fit the exported displacement vertices, comparing each summary field to its
matching reference at 1e-12 relative tolerance. Exported-vertex warpage must also
be finite and positive.

## Verification

Use a backend-enabled Release build, referenced here as `$build`, with the
dependencies listed above. Plain wrapped build/test commands:

```sh
cmake -S . -B "$build" -DCMAKE_BUILD_TYPE=Release
cmake --build "$build" -j2
OMP_NUM_THREADS=1 ctest --test-dir "$build" -R '^(case_run_schema_fixtures|odbcli_odbpp_cli_e2e)$' --output-on-failure -V
OMP_NUM_THREADS=1 ctest --test-dir "$build" --output-on-failure -j2
```

Release configure and full build exit 0. clang-format 23.1.5 `-i` followed by
`--dry-run --Werror` passes for the solver file, and `git diff --check` is clean.
The normal targeted CTest exits 0: 2/2 tests pass, including material schema
fixtures and the E2E (14.70 s).

For the negative and restored checks, the command is:

```sh
OMP_NUM_THREADS=1 ctest --test-dir "$build" -R '^odbcli_odbpp_cli_e2e$' --output-on-failure -V
```

| Local check (实测) | CTest exit | Maximum temperature error K | Reached stage |
|---|---:|---:|---|
| Original dotted fixture IDs | 8 | not reached | solve rejects the ID, exit 14 |
| Valid IDs, original stride of 6 | 8 | 3.8645886088048087 | solve exits 0, temperature assertion fails |
| Normal, valid IDs and corrected stride | 0 | 9.366658559883945e-10 | ranks=1/2 solve, readback, result.json |
| Reference heat flux temporarily ×1.01 | 8 | 0.41477719793664392 | ranks=1 solve exits 0, temperature assertion fails |
| Reference restored byte for byte | 0 | 9.366658559883945e-10 | ranks=1/2 solve, readback, result.json |

Only the analytical reference flux was multiplied by 1.01. The mutation is
absent from the committed driver; restoration was checked byte for byte before
the successful rerun (15.11 s). No assertion, numerical threshold, timeout or
skip policy was weakened. The cross-format fraction limit remains the brief's
1e-3; each format independently meets the 1e-6 analytical fraction gate.

Full Release CTest exits 8: **250 passed, 6 failed, 5 skipped** (261 total;
1423.45 s). E2E passes within this run (14.38 s). No test was excluded and no
timeout was increased. The four previously recorded failures reproduce:

| Test | Observed failure |
|---|---|
| `odbpp_map_placeholder` | 60 s timeout in the large-contour performance case |
| `ipc_fixture_present` | eight bare geometry-context classification assertions |
| `chain_ipc_uncut_chain` | expects copper-layer material `mat.fr4`; importer returns `mat.cu` |
| `ecaddiff_import_pair` | expects hard/discuss/info=11/0/5; receives 0/0/14 |

Two additional failures occur in this full run: `solve_ghost_scan` reaches its
300 s timeout, and `k1_preview_mpi_surface` aborts while parsing JSON. The latter's
retained stdout starts with a solder-material warning before the first JSON
event; its unchanged isolated retry passes in 44.34 s. Neither test executes the
modified solver pipeline. The unchanged FEM scan retry exits 8 after 177.23 s with a NumPy
`ValueError` reading a ragged matrix array (525 rows expected); it does not time
out on retry. After removing this run's 2.5 GB temporary matrix left by the timeout, a further
unchanged scan retry passes in 241.92 s under the original 300 s limit. That
retry records at least 3.96 GB free disk space and a peak matrix size of 2.66 GB;
disk pressure is a possible cause of the earlier ragged read, not a confirmed
code diagnosis. Its matrix generator and Python reader are outside this change.
The five skips are the two optional GPU tests and three assembly/STEP
placeholders. `event_log`, `solve_inputs`, `vtkhdf_results_mpi_2`, installation
and package checks pass. All-CTest-green acceptance remains unmet.

## Limits

This test excludes whole-board slots/holes, `--stack-override` thickness overrides,
cut geometry, and real boards. It does not change the whole-board rejection
expectations or importer policy. Backend-enabled self-hosted/nightly builds run
the numerical chain; hosted builds without the gated dependencies skip it.

The copper-thickness discrepancy remains [M1 #169](https://github.com/dzw05/chipcae/issues/169),
with the 30/35 µm exception pinned at relative 1e-12. The prior
[#82](https://github.com/dzw05/chipcae/issues/82) ruling excludes `copper_weight`
as a thickness source. The earlier dotted-ID blocker tracked in
[M3/material #168](https://github.com/dzw05/chipcae/issues/168) is addressed here
by using valid fixture IDs; no schema relaxation is needed.
