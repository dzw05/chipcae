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

Reports (`import_report.json`, `import_report.csv`, and the selected defects
file) are written before confirmation. Unconfirmed B-tier defects return 6
without writing the stack; C-tier defects return 7 even with acceptance.
Acceptance files contain `{"accept":[{"defect_id":"part/defect","note":"..."}]}`.
Unknown IDs produce warnings. Explicit `--accept` conflicts with
`--accept-b-tier` (exit 2). With neither option, `import.accept.json` beside the
assembly is used if present. Accept-all skips that default file entirely.

Part ID `void` is reserved (exit 14). Geometry threshold error prefixes map
through `protocol/errors.yaml`; unknown prefixes return INTERNAL (1).
STEP parts require an OCCT-enabled build; otherwise each part produces a
C-tier `OCCT_UNAVAILABLE` diagnostic. JSON support and ECAD support must be
configured for assembly imports. Error mapping reads the protocol scalar records
without adding a YAML library dependency to JSON-enabled builds.

This checkout still contains the SP0 throwing implementations of slicing,
placement and merge. Until the linked geometry target advertises support, assembly
treats the exact slicing/merge placeholder errors as empty results and skips
placement for empty slices, as permitted by the SP4 brief; it cannot produce part regions
or real slicing/merge diagnostics until SP2/SP3 land. All other errors propagate.

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
`CHIPCAE_ASSEMBLY_GEOMETRY_AVAILABLE` on the linked geometry target. Until those
implementations land, support defaults to OFF. Integration builds can explicitly
set `CHIPCAE_ASSEMBLY_ODBPP_AVAILABLE` or
`CHIPCAE_ASSEMBLY_GEOMETRY_AVAILABLE` as CMake cache options. Source changes do
not block configuration or require unrelated reader files to be linked.
ODB++ assembly support also requires ECAD to be enabled.
Imported product names also enforce the display-only
`NOT_CAE_GEOMETRY` rejection, including for ordinary STEP filenames and
unmapped bodies in `parts_from_step`.

Tests requiring SP2/SP3 geometry (thin-layer exit 6/acceptance, split reports,
and merged STEP regions) skip with code 77 while those implementations are stubs.
A STEP part requiring those operations is rejected with exit 7 and an explicit
unavailable-capability diagnostic. Exceptions from advertised implementations
propagate to the error report instead of discarding the part.
The main OCCT test build includes the synthetic STEP fixture generator; missing
fixtures fail the tests. Repair-policy and synthetic split/rewrite report tests
remain active without OCCT or SP2/SP3.
Expanded `parts_from_step` identities use the body name; the schema-required
`bodies` mapping `id` value does not rename a body.
