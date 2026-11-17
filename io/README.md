# io

结果写出、结果清单和检查点。

目标格式是 HDF5 / VTKHDF。在关闭 HDF5 依赖时，清单和检查点使用轻量文本占位实现，保证内核在最小配置下也能写出结果索引。

新结果接口和兼容旧接口的区别见下文。

## O1 results contract

`chipcae::io::ResultsWriter` (`vtkhdf_results.hpp`) accepts rank-local arrays without
MFEM dependencies. Enable `CHIPCAE_ENABLE_HDF5`; MPI support uses `MPI_COMM_WORLD`
when initialized. All ranks call the constructor, `write_step`, and `finalize` in
the same order; provide identical summary metadata on all ranks. The partition
count is the communicator size and stays fixed across steps. Never share a run
directory between concurrent writers. Existing finalized runs are rejected.

Each `<run>/out/step-<n>.vtkhdf` is a PartitionedDataSetCollection with Version
`[2,9]`, four indexed blocks and Assembly soft links in contract order: hex,
cut_poly, embedded_boundary, particles. The hex block is UnstructuredGrid; the
reserved cut block is an empty UnstructuredGrid and the last two are empty
PolyData. Nonempty reserved block inputs are rejected. Polyhedron face arrays
remain a documented TODO pending mesh/geometry agreement, including the shared
face `-(n+1)` convention.

Coordinates, T and U are float64 SI arrays. U has three interleaved components;
the solver supplies the exact eight Q2 vertex values (edge/face/interior DOFs
are omitted). T is scalar Kelvin. Cell layer_id/material_id/cell_kind are int32;
optional elem_gid is int64 (dense Morton column index times global nz plus k).
Connectivity is int64 and rank-local, VTK_HEXAHEDRON order. Counts contain one
entry per rank; concatenated Offsets restart at zero for every partition,
including empty partitions. Point/field lengths and IDs are checked collectively.

Parallel HDF5 uses MPI-IO and collective dataset writes; empty rank selections
use H5Sselect_none. With serial HDF5, MPI_Gatherv collects each array on rank 0,
which writes the same partition layout. This fallback costs rank 0 memory
proportional to global output size; MPI byte counts are bounded by INT_MAX.

`finalize()` writes `manifest.json.tmp` in the run directory, closes it, then
renames it to `manifest.json`. Existing readers see a complete old or new
manifest. Steps are published only after their HDF5 temporary files close and
rename successfully. The manifest uses `chipcae.results/1.0`, units SI, relative
step paths, field descriptors, four blocks, stackup.json, and summary entries.
The input chipcae.stack 0.2 JSON is copied verbatim as stackup.json; the caller
must validate it against schema/stack-0.2.schema.json. The legacy ResultManifest
and ResultWriter APIs remain available unchanged.

Every published step requires solver summaries warpage_pv (m), T_max (K), and
U_mag_max (m). warpage_pv must come from the solver's full Q2 field with the
best-fit plane removed; it cannot be reconstructed faithfully from this vertex
output. sigma_vm is reserved in the manifest and absent from step data.

Specification: https://docs.vtk.org/en/latest/vtk_file_formats/vtkhdf_file_format/vtkhdf_specifications.html
The contract's 2.9 minimum is ahead of the public specification's stated 2.8;
confirm the desktop reader baseline before enabling future polyhedron features.

### Generate S1 (binary output stays outside git)

```bash
source /workspace/prototypes/p1-mfem-thinlayer/env.sh
cmake -S . -B /workspace/build-o1 -DCHIPCAE_ENABLE_HDF5=ON \
  -DCHIPCAE_ENABLE_MPI=ON -DHDF5_ROOT=/workspace/hdf5-mpi
cmake --build /workspace/build-o1 -j4
NP=1 O1_BUILD=/workspace/build-o1 tools/samples/make_s1.sh
# Optional: NP=2 S1_RUN=/workspace/samples/s1-np2 tools/samples/make_s1.sh
```

The script copies P1 source into a disposable directory, applies the committed
p1-export.patch there, builds CPU MFEM, and solves the uncut trilayer at p=2,
n=8, nz=1 per layer, dT=-100 K. It exports exact vertex DOFs and the original
Q2 best-fit-plane top-surface PV. The S1 reference temperature is 293.15 K,
so output T=193.15 K. U_mag_max is the maximum norm over Q2 element nodes.
The patch exports stackup.json from the same solver layer definitions, using
chipcae.stack 0.2; s1.stack.json is the equivalent checked-in test fixture. Output defaults to /workspace/samples/s1/;
the script refuses to overwrite an existing path. For root OpenMPI execution
set OMPI_ALLOW_RUN_AS_ROOT=1 and OMPI_ALLOW_RUN_AS_ROOT_CONFIRM=1 as required
by the local MPI installation.

CTest runs independent h5py structure/dtype/empty-partition/manifest checks,
and vtkHDFReader checks when Python vtk is installed (`pip install h5py vtk`).
Missing optional Python packages are reported as skipped readback checks.
