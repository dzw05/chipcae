# ODB++ and IPC-2581 command line import

Configure ECAD support with `CHIPCAE_ENABLE_ECAD=ON`. The same material mapping,
stack conversion and binary polyset writer serve both formats.

```sh
chipcae-import board.tgz -o out/board.stack.json --material-map materials.json
chipcae-import board.xml -o out/board.stack.json --material-map materials.json --accept-b-tier
```

`--stack-override` and `--accept` are pending dispatcher integration and currently
return usage exit 2. The sections below describe their implementation in the
ECAD importer; these flags cannot yet be used through `chipcae-import`.

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
IPC conflict warnings instead use relative difference greater than `1e-9`; the
selected override value and its provenance are retained regardless of that threshold.

Material maps contain source-material-name to library-ID string mappings.
An explicit per-layer override material bypasses material mapping for that layer.
All other layers go through the existing material mapper. The report retains the
source material key independently of the selected ID. An explicit material override
is reported even when the source material key is empty. Without a material override,
an empty source key retains an empty report ID. Unmapped materials return 9.

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
| `Component`, `Package`, `PackageRef`, `Text`, `Attribute`, `Property`, `Material`, `Spec`, `EntryStandard`, `StandardPrimitiveRef` | B |
| ODB `misc/info`, `misc/attrlist`, `steps/<step>/layers/<layer>/components` (including `.Z` variants), and selected-step notices | B |
| Exact ODB `unsupported record T` diagnostic in `steps/<step>/layers/<layer>/features` | B |
| Copper symbols, traces, slots, via geometry, profile holes and other geometry losses | C |
| All other unknown ODB diagnostics, including bare names, feature/profile records and unlisted paths | C, conservatively |
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

The frozen main dispatcher currently rejects `--accept` and `--stack-override`
with usage exit 2 before calling the ECAD importer. It does parse their values
into `ImportOptions`, but its subsequent `reserved_argument` guard prints
`not implemented yet` and returns before dispatch. This differs from the brief's
assumption that parsing these options also enables forwarding. Their ECAD
implementation is present but unreachable through these CLI flags until the
dispatcher owner enables forwarding; changing that guard is outside this card's
frozen file scope.
`--accept-b-tier` already reaches the importer. `--t-min` and `--assembly` remain
unimplemented.

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
function and do not reject the import. The frozen archive reader does not strip
the fixture archives' common `odb/` directory, so they still fail the initial
`matrix/matrix` lookup. Thickness records must match the retained layer count
and order before the CLI consumes their provenance; a mismatch returns 7.

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

The public reader accepts the full ODB directory fixture. It resolves
straight-sided surfaces with retraced bridges into simple rings on the mapper's
existing database grid, preserving hole winding. Standard `r` aperture aliases
are translated to the mapper's round form. These steps preserve unsupported
drill diagnostics.

Full-board CLI acceptance remains blocked by actual unsupported geometry. The
owned fixture contains a plated oval slot, while the frozen `EcadModel::Via`
represents circular drills only. The ODB mapper reports `Slot:*`; IPC reports
unmodeled slot and pad/contour content. These are C-tier defects, so accepting
B-tier metadata cannot make these imports succeed. Plating thickness is also
absent; it must come from an explicit source, never a default. The exports use
different layer-name spelling (`F.CU` versus `F.Cu`), and their independently
rounded copper coordinates cannot satisfy exact sidecar hash equality while
preserving the source geometry and names.

CTest entries `odbpp_fixture_present` and `ipc_fixture_present` check fixture
presence and run the currently supported scenarios in their respective `odb`
and `ipc` groups. These registered groups already include success coverage:

- `report_and_preview` imports a supported ODB directory with exit 0, checks
  material IDs and thicknesses against the published stack through the layer
  report, checks custom report placement, and runs the preview smoke test.
- `unsupported_tiers_text` checks exit 6 without confirmation and exit 0 with
  `--accept-b-tier`; the IPC bare metadata cases check the same transition.
- `kicad_skipped_layers_reported` checks successful IPC layer-report output,
  including source function spelling, skipped-layer fields and absent defects.
- `via_plating_missing` checks IPC confirmation and successful B acceptance with
  stable defect IDs. The ODB group also runs native public-reader override and
  provenance assertions independently of the dispatcher.

They also assert the current prerequisite outcomes: full-board geometry is
rejected with C defects and exit 7 even with B acceptance; archives with a common
root directory are rejected with a missing `matrix/matrix`; and
unforwarded `--accept` / `--stack-override` flags return usage exit 2 without
publishing geometry or changing a previous layer report. Unexpected outcomes
fail CTest.

The registered groups print `FOLLOW-UP` only for strict success selectors that
depend on these prerequisites; the success cases listed above are executed.
`override_fill_missing` checks aggregate rejection, not successful override filling.
All other override scenarios, `acceptance` and `dispatcher_values` await
dispatcher forwarding. `cross_ipc`, `dir_and_zip` and `odb_skipped_layers_reported`
await full-board geometry/model support and archive root handling; `extension.*`
awaits archive root handling.
These are separate upstream follow-ups, not accepted CTest failures. No CMake
registration changes are needed. The public `full_board_reader` checks directory
reader composition separately and requires the unsupported slot diagnostic to
remain observable; it does not establish CLI acceptance.

The `report_and_preview` smoke test runs normally. Its material map supplies the
required homogenized dielectric ID `mat.fr4_eq`. For meshing, a separate preview
stack refines layer element counts to the thinnest layer's spacing; imported
geometry, thicknesses and artifacts are preserved. The preview's default cell-size
ratio limit is unchanged.

The unregistered `all` and named selectors retain every original success
assertion, including the blocked scenarios. Run `odbpp_e2e all` or a named selector (for example,
`odbpp_e2e odbpp_e2e_cross_ipc`) to inspect strict acceptance. Original geometry,
assertions and numerical thresholds remain enforced. Passing full-board
acceptance requires upstream model/reader work and a dispatcher brief amendment.

All examples are synthetic. Customer data, surveys and real-board measurements
must stay outside this repository.
