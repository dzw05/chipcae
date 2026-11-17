# Mesh preview (P0-1)

预览子进程。用于在不阻塞桌面界面主进程的前提下生成网格或结果的轻量预览。

Build with `CHIPCAE_ENABLE_JSONCONS=ON` and `CHIPCAE_ENABLE_HDF5=ON`.
Without either dependency the executable remains available and reports
`NOT_IMPLEMENTED` (exit 2). The schema path is bound to this source tree at
configure time; this entry point does not yet provide an installed schema layout.

```
chipcae-preview input.stack.json --nx 2 --ny 2 --levels 0 -o output.vtkhdf
```

`-o` accepts a stem or a filename ending in `.vtkhdf`. `--nx` and `--ny`
must appear together and be positive integers. `--levels` accepts a nonnegative
integer; 0 retains automatic hierarchy selection. Defaults come from `GridOptions`, except `max_ratio=10` for preview: the K1
FCBGA example has a 10:1 interface cell-height jump and fails the library
default of 2. The library default and stack layer counts remain unchanged.
Unknown options, duplicate options, malformed numbers, and extra input paths fail.
Every stdout line, including help, version, success, and errors, is a JSON object
with `stage`, numeric `progress` in [0,1], and string `message`. Diagnostics and
stack warnings go to stderr. Failures use the existing `INTERNAL` (exit 1);
no protocol codes were added.

`--polyset file` overrides the polygon data used for meshing. Stack loading still
validates all declared sidecars, including containment and digests. An explicit
polyset is also accepted for stacks without polygon copper (the builder uses only
referenced layers). Without the option, declared sidecars are loaded relative to
the input file and combined; they must share a database unit. Polygon copper
rasterization additionally requires the existing `CHIPCAE_ENABLE_ECAD` feature.

The existing writer always emits both the grid and an EB companion, including
an empty EB for the current null-cut pipeline. `--surface path` names the exact
EB filename: after the writer succeeds the CLI moves its companion to that path.
It never redirects the primary grid output. The destination directory must exist;
a move failure reports an error but may leave the successfully written grid and
default companion. This avoids changing the writer interface in P0-1.

Cell fields are `layer_id` and `material_id` (int32); the old names are removed.
Their existing compact layer mapping and material table values are preserved.
`cell_kind` remains 0 for regular, 1 for cut, and 2 for void cells.
