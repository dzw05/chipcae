# Mesh fixtures for solver tests

Include `chipcae/mesh/testing/mesh_fixture.hpp` and link `chipcae_mesh_fixture`.
Enable `CHIPCAE_ENABLE_JSONCONS`; the target is absent when JSON is disabled.
The library links core and the header-only JSON dependency, and uses schema and
background type headers. It requires no generator, cutting backend, MFEM, or
preview implementation.

The artificial D03 fixture is `tests/fixtures/mesh/i5_fcbga_small.mesh.json`.
It was generated from the unchanged `schema/examples/fcbga_demo.stack.json`
using `build_tensor_grid`, nx=ny=2, ranks=1, levels=1, max_ratio=16. Alignment
lines expand it to 9×9×6 intervals (486 cells). It contains no real case data.

```cpp
#include <chipcae/mesh/testing/mesh_fixture.hpp>

auto loaded = chipcae::mesh::testing::load_mesh_fixture(fixture_path);
if (!loaded) {
    // Report loaded.error().path and loaded.error().reason.
    return;
}
const auto& mesh = loaded.value(); // inject at the solver test's mesh input
```

Save overwrites the requested path deterministically. Load validates field types,
connectivity, array dimensions, and the existing mesh digest before returning a
complete mesh. Element attributes must be 1-based indices into `attr_table.entries`;
every boundary attribute must have a key in `attr_table.boundary_names`.
Face-set names are independent group labels: their membership in boundary names
is not validated. Face-set values retain the existing boundary index checks.
Optional `partitioning`, `node_layer`, and `node_column` arrays may be empty on
nonempty meshes; when populated, their lengths must match cells or nodes.
Errors use `INPUT_INVALID`; write failures use the existing `$` I/O path convention. Default empty meshes carry a null digest. The digest covers
geometry and connectivity, so it is not a checksum of every attribute or report.
The JSON layout is an internal test format, not a new I5 exchange contract.

The deterministic acceptance test also constructs a region/void input by hand:
dense column 0 is missing, column 1 retains one hex with `elem_gid=1`, and
attribute 2 selects the second `attr_table.entries` record using the frozen
consumer's 1-based indexing, relating it to its region, material, and body. The sample includes
boundary groups, pending cuts, copper data, frames, and nondefault reports.
This is a field stub for solver input tests; it neither generates cut geometry
nor validates SP5 or the real cutting pipeline. Solver integration belongs to
sp6; this library adds no solver or grid-generation API.

The frozen schema header owns the primary `chipcae::mesh::Expected` template.
The fixture header supplies its void specialization and the `chipcae::Expected`
alias locally to the testing API.
The fixture API uses `chipcae::Expected` as registered. Save returns
`Expected<void>` with a default-constructed success result.

The fixture directly reuses the inline digest in `preview/src/mesh_digest.hpp`
and its schema SHA-256 helper after validating connectivity and axis lengths.
This header inclusion does not link the preview implementation.
Nonempty copper layer maps have one entry per `attr_table.layers` entry; an empty
map remains permitted for meshes without this optional metadata.
