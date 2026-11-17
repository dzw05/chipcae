# VTKHDF volume reader contract v0.1

This versioned contract freezes the subset of solver results used by
`tests/integration/step_parts_e2e/parts_support.py::volume`. It supplements the
manifest/C1 output contract; it is not a contract for preview output or arbitrary
VTK files. The reader requires normal cut output (`cut_as_hex` disabled);
the fallback that exports cut parents as complete hexes is excluded. The producer is `io/src/vtkhdf_results.cpp`. Incompatible encoding or
geometry changes require a new reader contract version and updated consumers.

## Dataset and geometry guarantees

- Each manifest step file contains `/VTKHDF/hex` and `/VTKHDF/cut`, including
  empty blocks. These are VTKHDF 2.8 UnstructuredGrid blocks.
- Each block's `FieldData/material_names` is a string array shared across
  partitions, decoded as UTF-8. Cell material IDs are one-based indices into
  its flattened order. `CellData/material_id` is a one-dimensional array in
  the block's cell order. The reader requires this field in its solve case.
- `hex/NumberOfPoints` and `hex/NumberOfCells` are one-dimensional integer
  arrays of equal length, in partition order. `Points` has shape `(N, 3)` in
  metres, concatenated by partition. `Connectivity` is a one-dimensional
  integer array of exactly eight indices per cell, concatenated in the same
  partition and cell order. Indices are local to the corresponding partition's
  point slice, starting at zero; they do not index the whole concatenated array.
  Empty partitions contribute zero points/cells and no connectivity.
- Every hex cell is a complete axis-aligned rectangular hexahedron (VTK type
  12). Its exact volume is the product of its three coordinate extents.
  Cells intersected by cuts are represented in `cut`, not counted again as
  complete hex cells. Eight-entry connectivity alone does not guarantee this
  geometry for arbitrary VTK files; it is a restriction of this producer.
- `cut/CellData/volume` contains one float64 physical volume in cubic metres
  per cut cell, in exactly the order of `cut/CellData/material_id`. It is
  present for nonempty cut output; empty cut blocks may omit it. These are
  retained physical volumes, not background bounding-box volumes.

The reader sums complete hex volumes and retained cut volumes for the selected
material ID in each block. It advances the point and cell offsets independently
for every hex partition. It does not derive cut volumes from polyhedron topology.
Its preconditions are the solver producer above, C1 validation, and a requested
material present in the name tables; it must not be used for general VTK meshes.
