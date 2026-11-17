# S2 cut results example

Build target `s2_cut_minimal` with HDF5 and MPI enabled, then run:

```sh
mpiexec -np 2 /workspace/build-p1/io/s2_cut_minimal /tmp/s2-run examples/s2_cut_minimal/stackup.json
```

The stackup argument is copied as provenance; use the supplied `stackup.json`
for this example. Output is `<run>/out/step-0.vtkhdf` plus a manifest.
The two unit layers are assigned by element index modulo communicator size:
rank 0 has the complete base, rank 1 the top cut by x+y=1. This intentionally
exercises a rank without cut cells. It is an explicit results conversion example,
not the production column partitioner. T=300+x+2y+3z K, U=(.001,.002,.003) m.
Pass `--cut-as-hex` as the last argument for the parent-hex fallback.
The writer's rank-local conversion is shown beside the interpolation boundary;
production solvers sample their basis using point_parent_elem and point_xi.

CTest `cut_results_serial` and `cut_results_mpi_2` check rules 1–8, the committed
pre-change hex fixture, and VTK readback (`pip install vtk==9.7.2 h5py numpy`).
The copied external `tests/check2_reference.py` is sample-specific; its geometry
checks are adapted in `tests/check_cut_rules.py` to accept this example's output.
