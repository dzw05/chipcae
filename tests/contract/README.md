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
