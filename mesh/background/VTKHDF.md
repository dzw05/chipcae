# VTKHDF output

This API (`mesh/background/vtkhdf_writer.*`) is the mesh preview writer.
S2 solver results must reuse `io::ResultsWriter`, not build a second PDC writer.
See [the results contract](../../io/README.md): VTKHDF [2,8], blocks
`hex`, `cut`, `eb_surface`, `particles`, with nonnegative per-cell copied
outward faces and complete empty polyhedron templates. `--cut-as-hex` maps
to `ResultsStep::cut_as_hex` on that path.

Enable `CHIPCAE_ENABLE_HDF5`. Output is an independent UnstructuredGrid file
per rank plus a companion PolyData file, not collective parallel output.
Both have `/VTKHDF` Type/Version attributes and partition counts. Hex cells
use VTK type 12; all required cell fields use SI geometry and compact integer
IDs from the attribute table. `cell_kind`: 0 complete, 1 cut, 2 retained void.

Embedded quadrature points are PolyData vertices; each three-point triangle
rule also reconstructs a facet polygon. `normal` points outward from body;
`area_weight` is zero on facet-only vertices. Both files are written via
temporary paths and checked HDF5 operations; an error returns Expected.
No MPI types appear in the public interface. Input is the replicated
preparation mesh with `partitioning` selecting output cells and EB points.

Readback test is gated by HDF5. Validated locally against an in-worktree
HDF5 1.14.6 build; MPI output remains pending without MPI.
The root CMake dependency logic additionally requires a parallel HDF5 when
MPI is enabled, even though this writer does not require collective I/O.

Format reference:
https://docs.vtk.org/en/latest/vtk_file_formats/vtkhdf_file_format/vtkhdf_specifications.html
