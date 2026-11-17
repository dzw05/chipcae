# Assembly import report contract v0.1

This is the normative diagnostic-text contract for the JSON report requested by
`chipcae-import --assembly <assembly.json> --report <report.json>` when the input
uses `chipcae.assembly/0.1`. The input schema version selects this contract; this
contract does not require a report version field. It does not apply to the
separate ECAD `import_report.json` / CSV repair report.

The assembly CLI is an outstanding SP4 dependency in this tree. These requirements
define its acceptance behavior; they do not claim that the current importer
implements assembly reports.

## Required diagnostic content

On successful assembly import, the report must be a JSON object. Its diagnostic
string values must preserve the following content, after JSON escape decoding:

- Each imported part's assembly `id` must occur verbatim. This includes explicit
  `parts` entries and the mapped IDs in `parts_from_step.bodies`, rather than only
  the original STEP body names.
- If any imported part has `material_approx: true`, the report must contain the
  exact marker `近似材料` (approximate material). This marker is required even when
  the chosen material is a library material: it describes the assignment's
  approximation, not whether the material card exists.
- For the SP7 derived-lid fixture, whose part ID is `lid_chamfer_derived`, the
  report must contain the exact provenance marker
  `派生件（由真实盖板四角倒角，非原始设计）`. The fixture is derived from the real lid
  by chamfering its four corners; the marker must not imply an original design.

The markers above are stable machine-checked diagnostic tokens in v0.1, including
their punctuation. Additional explanatory text is allowed. JSON key names,
nesting, diagnostic order, whitespace, and JSON Unicode escaping are unspecified.
Consumers may search decoded JSON serialized with `ensure_ascii=False`; consumers
must not infer any other report structure or wording from this contract.

Removing or changing these guarantees requires a new contract version and an
explicit update of its acceptance consumers. The SP7 consumers are
`test_e2e_real_bulk_parts.py` (approximation marker and all five part IDs) and
`test_e2e_derived_chamfer_lid.py` (derived provenance marker). Numerical and
VTKHDF output checks remain governed by their separate contracts.
