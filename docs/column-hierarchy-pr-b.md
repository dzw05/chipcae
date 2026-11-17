# Coarsest column partitioning and line-subset coarsening

Background grids now select a hierarchy, pad uniform feature segments for axis divisibility,
partition the coarsest column graph once, and inherit ownership down to elements.
Element GIDs remain finest Morton column index times nz plus k. Coarse axes select every
other fine line; features are required only on the finest grid.

`GridOptions::levels=0` selects automatically; positive values specify total levels.
`max_coarse_columns=0` means no soft upper limit. Selection prioritizes 64 columns per
rank, then 16, then a single-level direct-solve report. Explicit infeasible levels return
HIERARCHY_INVALID. The grid report includes padding, axis subsets, dense column owners,
per-level maximum aspect ratio, warnings, and fine-cell partition imbalance (limit 0.25).
Optional `ar_warn` and `ar_max` default to infinity, since the baseline has no aspect
threshold configuration; both produce warnings only for the finest grid.

Uncut METIS column weights are 4^L*nz. The serpentine fallback omits this common weight
scale because its existing API rejects weight arrays. Cut-cell alpha weighting remains
deferred as permitted by the brief; the partition helper accepts a weights span.

## Measured old/new K1 values

Baseline: main b6e8134. Both builds: Release, default optional dependencies OFF,
same C++ compiler, serpentine fallback. Each case uses the existing `k1::stack()` and
`k1::options()` fixture, overriding nx=ny and ranks. No separate scalar named K1 exists
in the background test: reported values are cell count, rank loads, imbalance, and volume.
The original background fixture also reports volume=1 on both versions.

| nx=ny | np | old/new cells | old loads | new loads | old imbalance | new imbalance |
|---|---|---|---|---|---|---|
| 4 | 1 | 1024 / 1024 | 1024 | 1024 | 0 | 0 |
| 4 | 2 | 1024 / 1024 | 512,512 | 512,512 | 0 | 0 |
| 4 | 4 | 1024 / 1024 | 256,256,256,256 | 256,256,256,256 | 0 | 0 |
| 8 | 1 | 4096 / 4096 | 4096 | 4096 | 0 | 0 |
| 8 | 2 | 4096 / 4096 | 2048,2048 | 2048,2048 | 0 | 0 |
| 8 | 4 | 4096 / 4096 | 1024,1024,1024,1024 | 1024,1024,1024,1024 | 0 | 0 |

New total level counts for these rows: 2,1,1,3,2,2. Baseline partitions the finest grid.
Measurements and reproduction driver are in `/workspace/tasks/logs/colhier-b-*-measure*`;
commands are recorded in `colhier-b-last.md`. These are logical partition counts in the
CPU background test, without MPI or METIS enabled.

The nonuniform acceptance case has x padding=1 and y padding=0. It preserves the feature
at x=2.5 only on the finest grid, uses uniform subdivisions within each feature segment,
and checks max_ratio=2, even-line subsets, aspect diagnostics, parent/child ownership,
and finest dense index equality with independently Morton-sorted tensor columns.

Validation: Release build, all 11 CTest tests, K1 logical ranks 1/2/4, and clang-format
23.1.5 dry-run with Werror. No CUDA, nightly, CI, or protocol error definitions changed.
