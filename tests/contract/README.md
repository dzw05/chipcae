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

The `fixtures/fvm-io-1.0/check.py` acceptance checker runs without arguments
from any working directory. It discovers the repository from its own resolved
directory, then each ancestor, choosing the nearest directory containing
`tests/contract`; it never infers the repository from the working directory.
Copies outside the repository, including handoff packages, require
`--repo <source>` (otherwise exit 2 with `--repo required outside the repository`).
The shared `fixture_selfchecks(root)` hook runs all discovered checkers without
arguments, in sorted order with the repository as their working directory, and
includes captured stdout/stderr in any failure assertion.
Checkers must finish in bounded time, declare any third-party dependencies,
and produce deterministic output. Exit 77 reports a visible skip and lets the
hook continue with the remaining checkers. A green `contract_schemas` validates
fixtures; it does not establish product compliance or approve contract freezing.
This follows brief revision 2, which supersedes the original hook specification
requiring `--repo`;
existing checkers such as `cut-union-1.0` do not accept that option. The FVM checker
returns 77 (`SKIP`) when NumPy is absent; jsonschema is optional (prints
`SKIP-SCHEMA`, retaining structural checks). It checks synthetic laminate analytic
temperatures, an independent dense TPFA solve for 3×2×4 hexes, conservation,
completed-output manifest metadata and
the five required case fragments (`valid_fvm`, `valid_fvm_gamg`, `mixed_fem_fvm`,
`fvm_direct`, `fvm_column_gmg`), plus `solver_case` for case-sensitive solver rejection,
then tests `gen.py --out <directory>` determinism, overwrite behavior and checker
mutations. Run it directly or through
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

The checker independently fixes the diagnostic units to `1` and `W/(m·K)`,
so editing both metadata files cannot redefine them. Case shape checks cover
references, mesh dimensions, thermal/mechanical coupling, loads, constraints,
linear tolerance and output, following `field_compare/fixtures.py`. The package
also rejects independent invalid mutations, exercises the shared hook on a
copy with missing hex input, and accepts reordered fields, omitted `T_cell` and
changed finite summary values. Laminate temperatures use absolute tolerance
`1e-10 K`; hex temperatures use relative tolerance `1e-12`, with positive input
heat and relative energy imbalance at most `1e-9`. Field metadata and summary
methods compare exactly; active field names compare as sets.

The current product accepts FVM families, defaults to Jacobi, dispatches the
thermal pipeline and supports `volume`. Its unsupported-preconditioner error
text still differs from this draft, and mechanical summaries still use the
thermal-only placeholder. `ResultsPartition` has no optional `T_cell` member.
Elastic behavior follows I14; nonconvex geometry follows cutgeom-nc. Regenerated
file sets must match `REQUIRED` exactly. Scripts and structural JSON remain byte
identical; numerical golden JSON preserves exact keys, types, integer IDs and
ordering, while floating values use the existing `numerical()` comparisons
(laminate absolute `1e-10 K`, hex relative `1e-12`, conservation `1e-9`).
`gen.py` returns 0 on success or 2 for usage/I/O/dependency errors; `check.py`
returns 0 on success, 1 for contract mismatches, 2 for operational errors or 77
when NumPy is absent. Unsupported-preconditioner messages, `T_cell`, diagnostic
units and mechanical summaries still require architect and mesh/solver/vv
signatures before a separate freeze; this PR does not open that gate.

The FVM fixture checks every internal face against the distance-weighted harmonic
conductivity formula and pins the first x/y/z face conductances to hand-derived
values (`3/4000`, `1/2250`, `36/3125` W/K). `mesh_boundaries.json` records synthetic
mesh-array inputs and literal expected codes for zero z cells, empty attributes,
short/long copper arrays, valid copper data and an empty no-copper mapping.
Nonfinite axes, numeric attributes (including nested values) and copper fractions
must produce `INPUT_INVALID`; in-memory NaN/±Inf mutations check this directly.
`manifest_fvm_solid/prepublication.json` has empty steps, partitions and summary;
it is accepted before output publication and rejected as completed output.
