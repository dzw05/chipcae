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
attribute 2 relates it to its region, material, and body. The sample includes
boundary groups, pending cuts, copper data, frames, and nondefault reports.
This is a field stub for solver input tests; it neither generates cut geometry
nor validates SP5 or the real cutting pipeline. Solver integration belongs to
sp6; this library adds no solver or grid-generation API.

The frozen v0.3.2 header defines `Expected` in `chipcae::mesh`, unlike the brief's
`chipcae::Expected` spelling. These declarations retain the frozen namespace.
Because the primary template stores `variant<T, Error>`, the fixture header
supplies its `void` specialization with default success and error construction.
No schema declaration, namespace alias, or frozen contract is changed. The
namespace discrepancy should be reviewed by the interface architect.
