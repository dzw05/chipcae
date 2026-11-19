# Assembly import

```
chipcae-import --assembly assembly.json -o out.stack.json \
  [--polyset out.stack.polyset] [--report defects.json] \
  [--accept accept.json | --accept-b-tier]
```

Assembly input uses `chipcae.assembly/0.1`. Board, material map and STEP paths
are resolved relative to the assembly file. IPC-2581 and ODB++ use the public
ECAD readers. The output retains copper polygons for later copper-fraction
conversion. The polyset must share the output directory; its default name is
obtained by replacing the output's final extension with `.polyset`.
The standalone stack writer preserves existing copper reference paths verbatim,
including absolute paths and parent-directory references. It resolves relative
paths from the stack output directory only to read and hash the sidecar; the
same-directory requirement applies to the assembly CLI's `--polyset` output.
The stack loader independently enforces its case-root restrictions when loading
external references.

Reports (`import_report.json`, `import_report.csv`, and the selected defects
file) are written before confirmation. Unconfirmed B-tier defects return 6
without writing the stack; C-tier defects return 7 even with acceptance.
Acceptance files contain `{"accept":[{"defect_id":"part/defect","note":"..."}]}`.
Unknown IDs produce warnings. Explicit `--accept` conflicts with
`--accept-b-tier` (exit 2). With neither option, `import.accept.json` beside the
assembly is used if present. Accept-all skips that default file entirely.

Part ID `void` is reserved (exit 14). Geometry threshold error prefixes map
through `protocol/errors.yaml`; `outline.unsupported` maps to `CUT_UNSUPPORTED`
(exit 13), and unknown prefixes return INTERNAL (1). A missing assembly file
rejects the import with exit 7; malformed JSON and schema errors return 14.
STEP parts require an OCCT-enabled build; otherwise each part produces a
C-tier `OCCT_UNAVAILABLE` diagnostic. JSON support and ECAD support must be
configured for assembly imports. Error mapping reads the protocol scalar records
without adding a YAML library dependency to JSON-enabled builds.

Assembly calls the SP2 slicing and SP3 placement/merge implementations directly.
Invalid or empty STEP shapes produce C-tier defects and reject the import even
with `--accept-b-tier`. Accepted unmapped entities are skipped.

Assembly reports preserve the shared geometry report fields and CSV contract.
The assembly JSON extension adds `details` to each defect and a `parts` object
containing `split_layers`, `rewrite_targets`, and `material_approx`; the final
`import_report.json` is written from the shared report data and updated when a failure changes the final diagnostics.

The build copies assembly/stack schemas and the error protocol into
`import-assets` beside the build outputs. Installation packages them under
`share/chipcae`, resolved relative to the executable, so assembly
imports do not require the source checkout.
Until E-ODB-3 supplies an implemented reader, ODB++ assembly boards reject with
`odbpp reader not yet available`. Implementation owners declare support through
`CHIPCAE_ODBPP_AVAILABLE` on the linked ECAD target and
`CHIPCAE_ASSEMBLY_GEOMETRY_AVAILABLE` on the linked geometry target. Capability
options default to OFF unless advertised by their owner. Integration builds can explicitly
set `CHIPCAE_ASSEMBLY_ODBPP_AVAILABLE` or
`CHIPCAE_ASSEMBLY_GEOMETRY_AVAILABLE` as CMake cache options. Source changes do
not block configuration or require unrelated reader files to be linked.
ODB++ assembly support also requires ECAD to be enabled.
Imported product names also enforce the display-only
`NOT_CAE_GEOMETRY` rejection, including for ordinary STEP filenames and
unmapped bodies in `parts_from_step`.

Enable `CHIPCAE_ASSEMBLY_GEOMETRY_AVAILABLE` for geometry acceptance tests
(thin-layer exit 6/acceptance, split reports, and merged STEP regions).
The main OCCT test build includes the synthetic STEP fixture generator; missing
fixtures fail the tests. Repair-policy and synthetic split/rewrite report tests
remain active without OCCT.
Expanded `parts_from_step` identities use the body name; the schema-required
`bodies` mapping `id` value does not rename a body.

Assembly board, material-map, and STEP input paths all follow the frozen
assembly schema: they are relative to the assembly file and cannot contain
`..` components or be absolute paths. Schema-valid paths may resolve through
symlinks to shared files outside the assembly directory.
