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

Save overwrites the requested path deterministically. Load validates all fields,
connectivity, array dimensions, and the existing mesh digest before returning a
complete mesh. Errors use `INPUT_INVALID`; write failures use the existing `$`
I/O path convention. Default empty meshes carry a null digest. The digest covers
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

The frozen v0.3.2 header defines `Expected` in `chipcae::mesh`, unlike the brief's
`chipcae::Expected` spelling. These declarations retain the frozen namespace.
Save returns `Expected<void>` with a default-constructed success result. The
fixture header supplies the void specialization without changing the frozen
primary template.

The fixture directly reuses the inline digest in `preview/src/mesh_digest.hpp`
and its schema SHA-256 helper after validating connectivity and axis lengths.
This header inclusion does not link the preview implementation.
Nonempty copper layer maps have one entry per `attr_table.layers` entry; an empty
map remains permitted for meshes without this optional metadata.

Test-first evidence is commit `6129bd3e46f4e42668d812f9c9df855f5d321d62`,
which supplied NOT_IMPLEMENTED save/load stubs. Runtime failures are recorded in
`/workspace/tasks/logs/i5-fixture-red.log`; the implementation followed in
`d06d8382610b26d49820677b50fdcc65ab2a98ed`.
