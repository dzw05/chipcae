# K10 parallel and coupling MVP

Build from the repository root, with all artifacts inside this worktree:

```sh
source /workspace/deps/kokkos-env.sh
cmake -S . -B build-a3-kokkos -G Ninja -DCHIPCAE_ENABLE_KOKKOS=ON
cmake --build build-a3-kokkos
ctest --test-dir build-a3-kokkos --output-on-failure
```

`par` compiles every new public header and checks 2/4-part column ownership,
repeatable partitions, 2/4-rank integer and double halo exchange and bounce,
portable host staging, and thermal/mechanical ordering, failures and cutbacks.
Checks remain active in release builds. `par_public_headers` guards public
headers against native transport types/includes. Existing tests run alongside.
The fallback capability assertion targets this CPU-only, MPI-disabled build.

## Contracts and MVP limits

- Structured hex ordering is `(k*ny+j)*nx+i`. Columns contain `nz` cells,
  so default graph vertex weights are equal. METIS requires explicit
  `-DCHIPCAE_ENABLE_METIS=ON` (default OFF); use `METIS_ROOT` for a custom
  installation. Enabling it without METIS is a configure error. With it OFF, a deterministic serpentine
  walk through the four-neighbor column graph is divided into balanced chunks
  (column counts differ by at most one). Entire columns remain together.
  Optional positive column weights must match `nx*ny`; custom weights require
  METIS and are rejected by the fallback.
  Parts must be between one and the number of columns; empty grids are errors.
- Halo plans own host index tables. Peers are sorted by rank, while the mesh's
  index order is preserved. Duplicate receive destinations are rejected.
  `exchange` is an in-process collective transport, not a distributed backend.
  All sends are packed before receives; rank order fixes delivery order.
  Host pack/unpack kernels copy object representations without arithmetic.
  Device data uses synchronous Kokkos deep copies through explicit staging
  buffers. Integer/double payload tests include signed zero and NaN payloads.
- GPU capability delegates to A1's runtime communicator query. Staging uses
  SharedHostPinnedSpace under CUDA/HIP and HostSpace on CPU.
  Staging remains available even when GPU capability is true.
- The process driver uses the existing Solver::advance contract and architecture
  Status/ErrorCode fields. Both solvers must use the same mesh and shared context;
  mechanical consumes thermal state directly. No field mapping is performed.
  Failed results propagate immediately. A cutback reduces dt using a positive,
  smaller `dt_next_hint`, or halves dt, with minimum dt and retry limits.
  Callers must provide save/restore hooks for both solver and shared-field state
  to permit retries; without hooks a cutback returns Failed/INTERNAL. Save runs
  once before each attempted interval; restore runs on each rejected attempt.
  Retries rerun thermal before mechanical. Successful reduced intervals continue
  at the reduced dt until the full LoadStep is covered. Solver hints on converged
  results do not grow dt. Driver exceptions propagate to the caller.
