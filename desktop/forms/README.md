# Schema forms (D05)

`chipcae::desktop::SchemaForm::load(schemaFile, Json)` installs a draft 2020-12 schema and
an in-memory JSON value. A successful load can have validation errors: inspect `errors()`
and `validationChanged`. `document()` returns the current value; `documentChanged` reports
successful edits. `setObjectPath(JSON_pointer)` limits the editor to a subtree while validation
continues against the entire document. `setValue(pointer, value)` changes an existing value and
validates it; use `setObjectPath` afterwards to refresh programmatically changed widgets.

`CaseOutline(form)` binds selections to that editor. `setCaseDocument`, `setStackDocument`, and
`setResultDocument` accept separate schema/document pairs. Stack layers are separate because
case.stack is a reference. Array nodes use optional ids as identity, with JSON pointers as the
fallback for older inputs; duplicate-id policy belongs to D06. The result root is initially disabled.
No file writes, job launch, preview, or result reader is included.

The current repository delivers `schema/case-1.0.schema.json`, not `case.schema.json`;
`caseSchemaPath(repository)` handles both names. `result.schema.json` is not delivered and
loading it reports an explicit file error. The MainWindow dock uses the illustrative repository
case/stack examples, not production material data. `samples/cases/s1.json` is absent; tests use
the delivered `schema/examples/fcbga_demo.case.json` and thermal/stack examples. The material library currently fails schema compilation because
`material.v1.schema.json` references an undefined `$defs/frame`; that explicit failure is tested.
URI `$id` resolution is tested with an independent fixture.

Objects, optional/required presence, arrays, tuple items, const, enum, defaults and object oneOf
variants have generic widgets. Range/pattern/required/oneOf/if-then/contains/additionalProperties
and other schema assertions are evaluated by jsoncons on load and edits. All errors retain JSON
pointer paths, including nested oneOf diagnostics. Invalid intermediate numeric text remains in
the editor with an error; the last parsed JSON value is retained. Switching object variants seeds
that branch's required fields/defaults and preserves x-* payloads; it does not coordinate thermal
mode changes across other document fields. Those conflicts remain visible through validation.

Hidden x-* values remain in memory; explicit property annotation `x-chipcae-visible: true` opts
an extension into the editor. Values use stored units (SI/K); this component does not convert units,
adapt YAML, resolve case data-file references, or apply library/document-level semantic rules.
The parameters schema is still marked provisional upstream, so this form only exposes its actual
linear solver declarations. Mesh fields are ordinary schema values.

The resolver uses local *.schema.json files in the schema's directory and the repository material
schema directory, indexed by retrieval URL and $id. References retain their originating document
context. Unresolved references fail explicitly; no network retrieval occurs. Recursive editor
expansion is limited to 32 levels with an explicit unsupported message. The widget mapping is for
the delivered schemas: combinators that only assert conditions do not create special-purpose
controls; present values without a declared widget mapping use a generic JSON value editor.
