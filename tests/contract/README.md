# Cross-module acceptance

`contract_output_solver` runs the shipped FCBGA demo at reduced mesh resolution, requesting T in addition to its selected fields,
through the public solver CLI. `contract_output_desktop` uses the desktop's
`make_samples.generate_run` default fixture. Both validate the same
`schema/manifest-1.schema.json` and independently inspect their VTKHDF files.
Reserved fields are declarations for future output and are excluded from the
stored field-name bijection. T and U use K and m as enforced by the schema;
VTKHDF has no separate per-array unit attribute in this contract.

`desktop/tests/fixtures/s1b/manifest.json` is the manifest from
`/workspace/samples/s1b` (2026-11-17), retained in the repository for the desktop
reader contract. Its binary output is not committed. Mesh metadata is optional
for these older desktop fixtures; when present all six mesh properties are
required. Solver output additionally requires mesh metadata and a nonempty step
list. A manifest published before output may have empty steps and summary.
The desktop's deliberately obsolete `s1_compat` variant is outside this current
contract (it omits partitions and uses VTKHDF 2.9).

`contract_ecad_stack` reads the repository's minimal two-layer IPC-2581 board,
applies an explicit illustrative material mapping into the unchanged demo
material library, then validates the emitted stack and independently decodes
its polyset according to `schema/polyset-0.1.md`. The mesh consumer loads those
same files, enumerates feasible hierarchy levels, and builds a grid. The grid
API does not take a material library, so the harness checks all base and metal
material references against that library before invoking the consumer.

Output tests return 77 if h5py/numpy or the required solver backend is absent.
The ECAD test is registered when ECAD and JSON schema support are enabled.

The `fixtures/fvm-io-1.0/check.py --repo <source>` acceptance checker
requires NumPy; jsonschema is optional (prints `SKIP-SCHEMA`, retaining structural
checks). It checks synthetic laminate analytic temperatures, an independent dense
TPFA solve for 3×2×4 hexes, conservation, completed-output manifest metadata and
five case contract outcomes, then tests `gen.py --out <directory>` determinism,
overwrite behavior and checker mutations. Run it directly or through
`python3 tests/contract/validate_contracts.py schemas <source>` / `contract_schemas`.
Fixture IDs use `((k*ny)+j)*nx+i`; they are local keys, distinct from product
Morton `elem_gid`. Add `--handoff <package>` to compare repository/handoff SHA-256
maps. The optional-schema test also checks missing-T rejection without jsonschema.
The fixture input format uses `x_m`, `y_m`, `z_m` face-coordinate arrays,
`cells` with integer `id`, explicit `ijk` and `conductivity_diag`, and `boundary`
with `bottom_T_K`, `top_q_in_W_m2` and `sides: "insulated"`; laminate inputs also
have `layers` with `thickness_m` and `conductivity_W_mK`. Numerical expected files
have `cells` with `id` and `T_K`; root `expected.json` has `fields` metadata
(including optional `T_cell`) and `cases` mapping names to null or code/message.
The draft fixture declares diagnostic units `1` and `W/(m·K)`; the current
writer emits empty units, so architect approval and a consumer patch remain
pending. These tests validate fixture contract outcomes, not product parser or
solver behavior. Generate the complete package with
`python3 tests/contract/fixtures/fvm-io-1.0/gen.py --out <directory>`; the output
includes both scripts and stable UTF-8/LF JSON with finite numbers only.
