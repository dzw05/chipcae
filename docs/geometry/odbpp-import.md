# ODB++ and IPC-2581 command line import

Configure ECAD support with `CHIPCAE_ENABLE_ECAD=ON`. The same material mapping,
stack conversion and binary polyset writer serve both formats.

```sh
chipcae-import board.tgz -o out/board.stack.json --material-map materials.json
chipcae-import board.xml -o out/board.stack.json --material-map materials.json \
  --stack-override thickness.json --accept-b-tier
```

ODB++ inputs are directories containing `matrix/matrix`, or files ending in
`.tgz`, `.tar.gz` or `.zip`, with ASCII case-insensitive extensions. Other ECAD
inputs go to the IPC reader. Format detection does not inspect file content.
Archive safety, decompression and fatal parser errors come from the existing
ODB archive reader. No external extraction program is invoked by the importer.

## Layer thickness and materials

An override is a separate UTF-8 JSON file:

```json
{
  "schema": "chipcae.stack_override/0.1",
  "source": "Thickness measurements from the board designer",
  "layers": [
    {"name": "DIELECTRIC_1", "thickness_m": 0.00028, "material": "mat.fr4"}
  ]
}
```

`schema` and `layers` are required. Each entry requires a nonempty `name` and a
finite positive numeric `thickness_m`. `material` is an optional nonempty material
library ID. `source` is optional explanatory text, preserved verbatim in the
layer report. Unknown keys, duplicate JSON keys, malformed files, and duplicate
layer names after ASCII case folding are rejected. Empty `layers` is valid.
Unicode characters are not case folded. Names retain their original spelling in
reports. A name absent from the retained board layers produces one warning.

IPC and ODB overrides replace matching source thicknesses; differing positive originals
produce a warning. Every retained layer must have a finite positive thickness;
missing values are rejected together with all affected layer names. No default
thickness or copper-weight conversion is used.

The ODB reader enforces override precedence after mapping, without changing the
frozen mapper. A matching override always has override provenance, including when
it equals the source value. Only a differing positive ODB original produces
`odb_value_m` and a conflict warning. The exact override double is retained.
"Differing" means a different double representation for these finite positive
thicknesses, as required by the import comparison contract. A one-ULP difference
(including values produced by different arithmetic expressions) is intentional
provenance information, not numerical noise to discard with a tolerance. Equal
source and override doubles produce no original-value field or conflict warning.

Material maps contain source-material-name to library-ID string mappings.
An explicit per-layer override material bypasses material mapping for that layer.
All other layers go through the existing material mapper. The report retains the
source material key independently of the selected ID. Unmapped materials return 9.

The override parser is schema-specific and has no optional JSON library dependency.
Explicit accept-file support uses the shared `read_accept_file`, which requires
`CHIPCAE_ENABLE_JSONCONS=ON` in this checkout.

## Unsupported elements and confirmation

Both formats use this classification table. Entries contain names and relative
archive paths, never the input filesystem path, in their deterministic IDs.

| Diagnostic | Treatment |
| --- | --- |
| `SkippedLayer:<name>:<function>` | Layer report row only; no defect |
| `SolderMask:*` | B; suggestion `solder mask not modeled` |
| `LayerFunction:*` | B |
| `ViaPlatingMissing:<start>:<end>:<count>` | B; size is the count |
| ODB `ViaPlatingMissing:<drill-layer>` | B; size 1, because the diagnostic supplies no count or span |
| Known metadata, attributes, component/package references and text | B |
| ODB feature record `T` | B |
| Copper symbols, traces, slots, via geometry, profile holes and other geometry losses | C |
| Unknown records inside ODB features or profile paths | C, conservatively |
| Bare IPC extension names without geometry context | B; the reader supplies no spatial context |

Generic IDs use `board/<percent-encoded-diagnostic>/<source-line>`, with a
per-ID occurrence suffix for repeated entries. Encoding is
injective and independent of standard-library hashing and source relocation.
IPC via-plating IDs use `board/via_plating_missing/<start>-<end>`, following the
shared ECAD `board/` prefix contract. Repeated final IDs receive `/1`, `/2`, etc.
ODB's shorter diagnostic uses its generic `board/` ID.

B defects require `--accept-b-tier` or an explicitly named
`--accept import.accept.json` with entries of the form
`{"accept":[{"defect_id":"board/..."}]}`. IDs match exact bytes, including case;
duplicates are deduplicated by the shared accept reader. No accept file is
automatically discovered. Supplying both flags returns 2. C defects always return
7, regardless of acceptance or the ordering of B and C defects. Unconfirmed B
returns 6. Import rejection returns 7; success returns 0 after output publication.

The main dispatcher passes `--accept` and `--stack-override` to the ECAD importer
for both ODB++ and IPC-2581 inputs. STEP's reserved-option behavior is unchanged;
`--t-min` and `--assembly` remain unimplemented.

## Reports and output publication

`import_report.json` and `.csv` contain actual defects, including confirmation
and rejection results. `defects.json` retains its existing summary format and
empty `defects` array. Use `import_report.json.defects` to inspect individual
records. `--report path` changes only the summary's destination.

`import_layers.json` is written beside the output stack, independently of
`--report`. Its `layers` array contains retained layers in stack order, followed
by skipped layers sorted by source name and function. Each row has `name`,
`status`, `function`, `copper`, `material_key` and `material_id`. Skipped rows have
false `copper` and empty material strings, and omit all thickness fields.
Retained rows additionally have `thickness_m` and `thickness_source`, which is
`ipc`, `odb`, or `override:<override-file-basename>`. Differing positive ODB
originals have `odb_value_m`; equal and missing originals do not. The top-level
`source` text is present only when supplied. `warnings` contains override and
acceptance warnings; the same messages are printed to stderr.

ODB function spelling is recovered by a best-effort second read of
`matrix/matrix`. Missing report-only lookups retain the mapper's normalized
function and do not reject the import. Inputs with an `odb/` archive root fail
the initial parser read; they do not reach this metadata recovery step. Thickness
records must match the retained layer count and order before the CLI consumes
their provenance; a mismatch returns 7.

Stack, binary polyset and layer reports are staged before publication. The layer
report is published last; publication failures restore previous output files.
Usage, confirmation, import and mapping failures preserve any existing layer
report and do not publish a new stack.

## Limitations and comparison

Only the existing ODB parser and mapper's supported board geometry is modeled;
unsupported content is surfaced through the table above. Silk/paste are skipped;
solder mask is not modeled. Multiple steps and omitted components require review.
Via coordinates remain in the imported model and are not serialized into the
stack/polyset pair. Preview consumes the successful stack through its existing
pipeline.

Cross-format acceptance compares stack keys recursively, thickness/outline at
relative tolerance 1e-6, and decoded copper areas at 1e-3. Model via positions
must agree within one database unit. Exact binary hashes additionally require
identical polygon discretization and ring representation. The frozen readers
may discretize arcs differently; this conflicts with strict hash equality and
cannot be resolved by silently weakening the acceptance assertions.

The full integration fixture still needs upstream compatibility fixes. Its archives
contain an `odb/` root directory that ArchiveReader does not strip. The directory
input is rejected for an invalid/degenerate `F.CU` contour, and its IPC export
reports unsupported board geometry. The preview smoke material map uses `mat.fr4`,
while the stack loader requires homogenized material IDs to end in `_eq`.

The existing CTest entries `odbpp_fixture_present` and `ipc_fixture_present`
run all scenarios in their respective `odb` and `ipc` groups. The nine scenarios
below are ordinary acceptance tests with success expectations. A prerequisite
failure fails the containing CTest entry; no expected-failure gate is applied.

| Scenario | Current prerequisite failure |
| --- | --- |
| `cross_ipc` | Archive root |
| `dir_and_zip` | ODB copper contour |
| `extension.TgZ`, `extension.TaR.Gz`, `extension.ZiP` | Archive root |
| `odb_skipped_layers_reported` | ODB copper contour |
| `override_ipc`, `override_empty_ipc` | IPC board geometry |
| `report_and_preview` | Preview material ID |

The frozen baseline dispatcher also rejects `--accept` and `--stack-override`
before calling `run_ecad`, even though it parses their paths into `ImportOptions`.
The ECAD implementation consumes these fields, but the corresponding CLI tests
remain failing until the dispatcher prerequisite is resolved upstream. Restoring
its baseline behavior is required by this card's file freeze.

Run `odbpp_e2e all` or any named selector (e.g.
`odbpp_e2e odbpp_e2e_cross_ipc`) to inspect failures individually. All original
geometry, assertions and thresholds remain enforced. Full-board cross-format
acceptance is not complete while these ordinary regression tests fail.

All examples are synthetic. Customer data, surveys and real-board measurements
must stay outside this repository.
