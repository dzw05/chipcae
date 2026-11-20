# Mesh preview (P0-1)

预览子进程。用于在不阻塞桌面界面主进程的前提下生成网格或结果的轻量预览。

Build with `CHIPCAE_ENABLE_JSONCONS=ON` and `CHIPCAE_ENABLE_HDF5=ON`.
Without either dependency the executable remains available and reports
`NOT_IMPLEMENTED` (exit 2). The schema path is bound to this source tree at
configure time; this entry point does not yet provide an installed schema layout.

The Python preview acceptance tests require NumPy and h5py in the interpreter
selected by CMake (`Python3_EXECUTABLE`).

```
chipcae-preview input.stack.json --nx 2 --ny 2 --levels 0 -o output.vtkhdf
```

`-o` accepts a stem or a filename ending in `.vtkhdf`. `--nx` and `--ny`
must appear together and be positive integers. `--levels` accepts a nonnegative
integer; 0 retains automatic hierarchy selection. Defaults come from `GridOptions`, except `--max-ratio R` defaults to 10 for preview (finite R >= 1): the K1
FCBGA example has a 10:1 interface cell-height jump and fails the library
default of 2. The library default and stack layer counts remain unchanged.
Unknown options, duplicate options, malformed numbers, and extra input paths fail.
Every stdout line, including help, version, success, and errors, is a JSON object
with `stage`, numeric `progress` in [0,1], and string `message`. Diagnostics and
stack warnings go to stderr. Progress and success events come only from MPI rank 0;
errors may come from any rank. The success `message` is itself valid JSON with
`grid`, `max_ratio`, `L` (effective finest level, zero based), and `n0` (effective
coarse grid dimensions, `AxB`). Use the same ratio in preview and solve to reproduce
the mesh. Mesh failures use the shared solve error mapping: unsupported cuts
exit 13, invalid outline input exits 14. Other failures use `INTERNAL` (exit 1).

`--polyset file` overrides the polygon data used for meshing. Stack loading still
validates all declared sidecars, including containment and digests. An explicit
polyset is also accepted for stacks without polygon copper (the builder uses only
referenced layers). Without the option, declared sidecars are loaded relative to
the input file and combined; they must share a database unit. Polygon copper
rasterization additionally requires the existing `CHIPCAE_ENABLE_ECAD` feature.

The existing writer always emits both the grid and an EB companion, including
an empty EB when there are no cuts. `--surface path` names the exact
EB filename: after the writer succeeds the CLI moves its companion to that path.
It never redirects the primary grid output. The destination directory must exist;
a move failure reports an error but may leave the successfully written grid and
default companion. This avoids changing the writer interface in P0-1.

Cell fields are `layer_id` and `material_id` (int32); the old names are removed.
`layer_id` is the stack layer order + 1. Stack 0.2 has no material table:
`material_id` follows tensor_grid's 1-based first-appearance order of material
names. The grid's `VTKHDF/FieldData/layer_names` and `material_names` string
arrays map index i to ID i+1, using the same attribute-table source as the cell IDs.
`cell_kind` remains 0 for regular, 1 for cut, and 2 for void cells.

By default, inline `outline.polygon` is clipped using the same convex half-space API
as solve, after building the full bounding grid. Clockwise rings and repeated
closing points are accepted. Non-convex or degenerate rings are rejected; vertices
must be finite and inside `outline.box`. Clipping failures emit an error event
before output is written. Holes are unsupported.

`--nonconvex-outline` enables simple non-convex outlines through the mesh/cut
general entry point, using convex decomposition and per-cell union. The option is
off by default to keep preview consistent with `chipcae-solve`; the default will
change together with solve's non-convex outline support. Self-intersecting outlines
still exit 13, and holes remain unsupported. Convex and empty outlines delegate to
the default cutter and retain identical output.

Regions with a `footprint.polygon` are clipped in the same pass as the outline:
`region_cut_planes` supplies the extra planes to the selected cutter. Errors use
the shared mapping: `CUT_UNSUPPORTED` and `outline.unsupported` exit 13;
other clipping errors, including `INPUT_INVALID`, exit 14.

The success event reports `cell_count`, `cut_cell_count`, and `mesh_sha256` after
clipping. `cut_cell_count` agrees with the number of `cell_kind=1` entries in
VTKHDF; `vol_frac` contains the retained volume fractions. Grids with no outline
or region polygons have zero cut cells. Use identical integer grid parameters and
ratio settings to compare preview and solve digests.

An external polyset layer named `outline` without an inline polygon is unsupported:
preview exits with `CUT_UNSUPPORTED` (13) and explains that an inline polygon is
required. It does not silently preview the bounding box in this case.
