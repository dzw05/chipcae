# Field comparison

`tools/vv/field_compare.py` compares first-step temperature and displacement
fields from two result directories. It reads `manifest.json` and its first
VTKHDF step with Python, NumPy and h5py. Fixtures are synthetic; no measured
solver results are stored here.

```sh
python3 tools/vv/field_compare.py <run_dir_a> <run_dir_b> \
  --preset same-mesh-solver --t-ambient <ambient_K> \
  --remove-rigid --data-class real --case-id <case_id> -o <local_output.json>
```

Real results require explicit `--data-class real`. Both the JSON destination
and `<out>.md` must resolve outside every git tree, including through symlinks.
Stdout contains the verdict and output path. Existing output files are
overwritten; unrelated files are preserved. Enums accept lowercase only.

| Preset | Gates | Sources |
|---|---|---|
| `same-mesh-solver` | T and U maximum relative field error; warpage PV, T_max and U_mag_max relative QoI errors, each ≤ 1e-8 | `tests/solve/test_column_gmg.py:150-155`; issue #205 |
| `ranks` | Same five gates, each ≤ 1e-8 | `tests/integration/f1130_uncut_cli/test_uncut_cli_e2e.py:26`; `briefs/fvm-case-e2e.md:62` |
| `fem-fvm` | Only relative T_max temperature rise error ≤ 1e-2 | `handoff/rulings/fvm-solver-io.md:55` (other fields are diagnostic, line 57) |
| `report` | None; verdict `report` | Reporting only |

All gated presets require finite ambient temperature. Temperature errors use
reference temperature rise; displacement errors use Euclidean norms. Zero
reference scales produce JSON null and fail applicable gates. Report without
ambient still includes absolute temperature errors. Different mesh hashes
bypass field alignment and metrics, retaining QoIs with verdict `mesh_mismatch`.
JSON uses native round-trippable Python floats and forbids NaN/Inf.

Points merge by lexicographically sorted `round(x / tol)` keys, with
`tol = 1e-12 * bbox_diagonal`. Duplicate values are averaged; T spread is the
scalar range and U spread is the largest pairwise Euclidean distance. Cells
sort by `block * 2**62 + elem_gid` (hex=0, cut=1). IDs must be integral in
`[0, 2**62)`, unique within each block across all ranks. Ghosts are rejected,
including identical copies. Cell temperatures prefer `T_cell`, otherwise use
local connected vertex means before point merging. L2 uses reference volumes;
an incomplete volume array falls back to equal weights with a note. Maximum
ties select the first canonical item. Rigid motion removal independently fits
a centered, six-parameter least-squares system for each run.

The frozen arrays cannot express partially available point fields. Missing
block values can be filled by coincident points from another block; any merged
point still lacking a value is an input error. Missing cut cell temperatures
use internal NaN sentinels, excluded and counted in notes. Mixed nonempty
`T_cell`/`vertex_mean` block provenance needs an upstream definition for the
single source label and is rejected. Zero bounding-box diagonal and
nonpositive volumes are rejected. These conservative policies resolve the
plan's unspecified cases without inventing public enum values.

| Exit | Meaning |
|---|---|
| 0 | Pass or report |
| 1 | Gate failure |
| 2 | Usage error, including missing ambient or prohibited real output |
| 3 | Mesh mismatch |
| 14 | Invalid input or alignment |

The public library API is:

```python
load_run(run_dir: Path) -> RunFields
compare_runs(ref: RunFields, other: RunFields, *, preset: str,
             t_ambient: float | None = None, remove_rigid: bool = False) -> dict
ensure_output_allowed(out: Path, data_class: str) -> None
```

`RunFields` is a dataclass with, in order: `mesh_sha256`, `qoi`, `qoi_method`,
`points` (P×3), `T` (P or None), `U` (P×3 or None), `cell_gid` (C int64),
`cell_volume` (C or None), `cell_T` (C or None), `cell_T_source`, and
`dup_point_spread`. `UsageError` and `InputInvalid` derive from `ValueError`;
`GATES` contains the exact limits and source references.

`compare_runs` returns comparison content without case identity, data class,
or directory names. API callers must add `case_id`, `data_class`, and
`ref.run_dir`/`other.run_dir` (the corresponding `Path.name`) before serializing
the complete `chipcae.vv_field_compare/0.1` record. Check **both** destinations
with `ensure_output_allowed` using the record's final data class before writing
either file. CLI performs these steps itself.

```sh
ctest --test-dir <build_dir> -R '^field_compare_unit$' --output-on-failure
ctest --test-dir <build_dir> -R '^field_compare_(ranks|outline_bbox|fem_fvm)$' --output-on-failure
```

Unit tests need only NumPy/h5py. Solver tests use an 8×8 three-layer heated
stack, nonzero thermal rise and warpage, `rtol=1e-10`, and call the existing
`summarize()` once per successful solve before comparison. Measured diagnostics
are marked `实测`. Missing dependencies return 77. Outline testing must pass
plain-stack GMG/direct before the prescribed #205 skip. FEM/FVM returns 77
until its upstream pipeline exists. Elasticity absence then nulls U diagnostics
with `fvm elastic not wired`; the sole thermal gate remains mandatory.

FEM runs do not request `volume`: the loader accepts that explicit output field
only for FVM. FEM references without complete `CellData/volume` use the existing
equal-weight cell-temperature L2 calculation and emit the note
`cell_T L2 uses equal weights: complete reference volumes unavailable`.
FEM volume weighting is future M3 interface work
(`v-int-field-compare-fixture-volume`, 2026-11-21).
