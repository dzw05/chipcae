# K1 P1 patch log and design deviations

This log covers the must-fix work on the K1 chain (PRs #13–#18) against
the mesh-team review and the architect / K9 §3 rulings. Field inventory
was checked against the design page
https://cursor.com/canvas/store-cb1250dc-2b01-44d2-a2e6-225d0cab8b5b/82a66aed-852d-4f73-8340-72bdf38a23b7
and the local export `/workspace/specs/mesh-design-v0.2.canvas.txt`.

## Must-fixes

### K1-1
- Homogenized materials must end with `_eq`. Solder-like names (substring
  `sac` or `solder`) emit a warning and are not rejected. Examples use
  `organic_eq`, `SAC305_ball_eq`, and `silicon_eq`.
- `symmetry` other than `none` is an error at `$/symmetry`, not ignored.
- Schema negative cases are split: URL and missing sha256 are separate
  cases; each failure path and reason is asserted. A positive
  `SAC305_ball_eq` case is included.

### K1-2
- `node_layer` takes the lower layer at an interface; top-layer nodes stay
  in range.
- `node_column` is `int64` (`Index`).
- Columns are ordered by an in-house 2-D Morton code, then `k` within the
  column. `elem_gid = column_sfc * nz + k`; node GIDs follow the same
  rule. No libmorton. A test requires bit-identical GIDs on 1/2/4 ranks.
- `attr_frame`, `internal_interfaces`, `order`/`nodes`, and
  `HexMeshDevice`/`upload` are present.
- `interface:A|B` now resolves each side to a body set and keeps the
  shared face patches; overlapping or empty patches are errors. This
  replaces the earlier `bdr_attribute` typo path (M2-5).

### K1-3
- Embedded-boundary attributes are allocated above existing surface
  attributes and no longer reuse `BodyId` (no collision with exterior
  `bdr_attribute`).
- Faces exposed by deleting void elements use a dedicated attribute
  (`exposed:void`), not 7, and do not collide with `interface:L0|L1`.
- Isolated vertices left by void deletion are compacted; remaining nodes
  keep their original GIDs and coordinates.
- Level-set storage is a flat `View` with a memory-space template, not a
  View-of-Views.
- `void_attribute` defaults to `-1`.
- Host `CutLayerArrays` / device `CutLayerView` field names stay aligned
  with the frozen code activation interface (M3-6 activation types are
  unchanged; the design page is updated by the mesh team).

### K1-4
- Adapter tests now check a complete unique union of source `elem_gid`
  values across ranks and compare each local hex to the source
  coordinates for that GID (not a self-check of the adapter arrays).
- `create()` rejects out-of-range vertices, non-positive / non-contiguous
  attributes, and mismatched boundary lengths.
- Named face sets are installed on `bdr_attribute_sets`.

### K1-5
- VTKHDF read-back compares `Points`, `Connectivity`, and offsets, and
  checks cell-data values against the source arrays.
- Each output step writes one collective file (`stem.vtkhdf` and
  `stem.eb.vtkhdf`). Parallel HDF5 uses collective MPI-IO; otherwise
  rank 0 writes the already-replicated preparation arrays. Per-rank
  files are an explicit `debug_per_rank` option, default off. Tests
  run at 1/2/4 ranks when MPI is on.

### K1-6
- MPI patch tests run at 1, 2, and 4 ranks. See the Hypre note below.
- Optional-dependency jobs (schema / MFEM / HDF5 / MPI) are not run on
  PRs. They live in `.github/workflows/nightly-mesh-fulldep.yml`: Linux,
  weekdays, 45-minute timeout, and they run only when `schema/`, `mesh/`,
  the mesh tests, or the dependency script changed since the last
  successful run (or when `workflow_dispatch` forces a run). The cache
  key includes METIS 5.1.0, hypre 3.2.1, HDF5 1.14.6, jsoncons 1.4.0,
  MFEM 4.10, and the compiler identity.
- `nightly/run_gpu.sh` now requires a CUDA MFEM prefix, configures and
  builds, and exits 77 when no GPU is present. A missing build no longer
  exits 0.
- This file is the required patch log and deviation list.

## Architect / K9 §3 rulings

- Activation interface is frozen as in the code. `ActivationMask` and
  `resolve_activation` return element GID pairs. `GhostPenaltyCache`
  keeps the code field names. The cache hash is over the sorted unique
  set of activated element GIDs (`mask_hash(activated_gids(...))`).
- Face groups: no `faceset:body:<id>`. Embedded-boundary groups are
  `interface:<A>|void`. `void` is reserved; `void|void` is an error;
  `A|B` and `B|A` name the same group. `faceset:` is only for explicitly
  named stack face sets.
- `interface:A|B` sides may be layer, region, or body (not group). Each
  side is resolved with `resolve_interface_side`: a layer contributes
  only its base body (no Inclusions); a region contributes that
  Inclusion's body. Shared patches are reported with aligned vs cut
  counts and areas. Overlap or an empty patch set is an error.
- Ghost-penalty faces use an `int64` global face id, `(gid_minus,
  gid_plus)` with `gid_minus < gid_plus`, local face numbers 0–5 in
  MFEM Hexahedron order, and owner rank of `gid_minus`. Layout is SoA
  with flat Views.
- `layer:X` means only the layer base body on an interface side.
  In volume references (activation, deactivation, loads), `layer:X`
  means the whole layer including Inclusions. The two entry points are
  `resolve_interface_side` and `resolve_volume_ref`; both are tested.

## Thresholds

Values stay in `mesh/background/thresholds/k1.json` and are read by the
tests. `cell_budget` (50000) was moved there; the C++ default is
unchanged. `vf_min` is still 1e-8 and was not renamed (awaiting a
product decision). `partition_imbalance` is 0.25. `snap_tol` and
`snap_phi` are 1e-14 m absolute.

K1 no longer has a separate METIS option; partitioning uses
`chipcae::par::ColumnExtrudePartitioner` (METIS if that library was built
with it).

## Hypre initialization (M6-1)

The earlier GENERIC error was a test bug, not a hypre 3.2.1 or MFEM 4.10
defect. The tests called `MPI_Init` but not `mfem::Hypre::Init()`, so the
first `HypreParMatrix` (built inside `GetEssentialTrueDofs`) auto-initialized
hypre and left `HYPRE_ERROR_GENERIC` set; BoomerAMG then aborted on that
stale bit. This is the same issue as mfem/mfem#4943. The tests now call
`mfem::Hypre::Init()` after `MPI_Init` (see `runtime.hpp`). The version-gated
error exemption is gone. The patch test also checks `HYPRE_GetError() == 0`
after setup and after the solve, and caps AMG-PCG iterations with
`amg_pcg_max_iter` in `k1.json`.

## Round-2 additions

- Exposed faces created by deleting void cells are registered per body as
  `interface:<body>|void` and counted as aligned faces (N1). There is no
  `exposed:void` group.
- A layer, region, body, or activation group named `void` is rejected at
  load (N2).
- `InterfaceReport` lists `side_a_bodies` and `side_b_bodies` for each
  interface (N3).
- `cv_layout.hpp` now declares the design §3 CV types and is covered by
  the standalone public-header compile test.
- The mesh full-dep nightly cron is `0 18 * * 0-4` (02:00 Beijing time
  Monday–Friday). The change gate also watches `core/`, CMake files, and
  `tests/integration/mesh_p1/`.

## G1 evidence

Full-dependency logs are written under `/workspace/tasks/logs/k1-g1-fulldep/`.
The G1 gate also needs the first green nightly after merge (not done
here; nothing is merged in this work).

## Round-3 additions

- R3-1: resolving an `interface:A|B` reference records the user-written
  string in `preparation_report`, the body names each side resolves to,
  and aligned vs cut patch counts and areas. A mixed-reference test
  covers `interface:layer:base|body:cap` and
  `interface:layer:substrate|body:solder_*`.
- R3-2: deleted `chipcae::mesh::ColumnExtrudePartitioner`. The mesh takes
  the column→rank map from `chipcae::par::ColumnExtrudePartitioner` and
  expands it into the element partition array in Morton column index ×
  nz + k order. par's structured `(k*ny+j)*nx+i` order stays inside par
  and is not written to files, halos, or other external interfaces.
  The same mesh is checked at 1, 2, and 4 ranks: each element's rank
  equals its column's rank. par was not modified.
- Numbering: one global element and node GID rule, mesh's column Morton
  index × nz + k (nodes use nz+1).

### Carried-over nits (not fixed in this round)

- nit 4: nightly runs OpenMPI as a second, independent MPI
  implementation. G1 evidence is the first green nightly after merge;
  local MPICH logs are only supporting.
- nit 6: `GhostPenaltyCache` holds a full mesh copy. This must be
  changed to an attribute→gid map before G3 performance work or
  scale-up.

### Cheap nits fixed in this round

- Old nit 7: `vtkhdf_writer.cpp` no longer uses `thread_local` writer
  state. Rank 0 renames the final files only after `agree()`, then all ranks
  barrier before returning so readers see the final names.
  `MPI_COMM_WORLD` remains only because `Communicator` has no native
  handle; the writer checks that the snapshot matches world.
- Old nit 13: embedded-boundary attributes are packed compactly above
  the aligned-patch range, one id per body that actually has cut faces.
- patch test: the pre-Mult hypre check is labeled "after AMG construct";
  `SetMaxIter` uses `amg_pcg_max_iter`.
- `k1_public_headers_kokkos` compiles public headers with
  `CHIPCAE_ENABLE_KOKKOS` when Kokkos include dirs are available.
- Comment on `HexMeshArrays::face_sets`: cut-face attributes in
  `bdr_attribute_sets` are not attached to MFEM boundary elements;
  solvers must read cut faces from `CutLayerArrays`.
