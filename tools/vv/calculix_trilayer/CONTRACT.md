<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 Zhiwei Duan -->

# WP-2 REFERENCE artifact contract, version 1

A conforming artifact distribution MUST include `REFERENCE.json`,
`reference.schema.json`, `contract.py`, and this document together. The schema
is a structural component of the contract, not a standalone validity test.

Consumers MUST validate artifacts with `contract.py` and its `jsonschema`
dependency, either by calling `validate_reference(data)` or by running:

```sh
python3 contract.py REFERENCE.json
```

A successful return (CLI exit status zero) is required before accepting an
artifact. Schema-only validation MUST NOT be advertised as contract validation.
The validator requires top-level `W_bfp_m` to equal the `W_bfp_m` of the mesh
named by `selected_mesh` (which must be `fine`), in addition to the structural
schema checks. This equality applies to regenerated values as well as the
checked-in result; neither value is pinned to a particular solver run.

Standard JSON Schema draft 2020-12 cannot express equality between arbitrary
instance values. The required validator binds the selected name and top-level
value into a `contains` constraint before applying the schema. Distributing or
using the schema alone therefore does not implement this artifact contract.
