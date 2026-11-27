# Scheme A Python API contract

Import the public functions below and `InputInvalid` from
`tools.vv.fvm_warpage_scheme_a`. Other imported names and underscore-prefixed
helpers are implementation details. The module consumes supplied run data and
evaluations; it does not run a solver or supply a FEM evaluator.

## Validation and decisions

`InputInvalid` is a `ValueError` subclass for missing, malformed, ambiguous or
nonfinite input. The three top-level decision APIs are
`fem_reference_self_convergence`, `scheme_a_gate` and `scheme_a_preconditions`.
They convert **all `InputInvalid` validation failures**, including quantity
validation, into this failed result:

```python
{"pass": False, "checks": [
    {"name": "input", "value": None, "limit": None,
     "pass": False, "reason": "<nonempty diagnostic>"}
]}
```

The reference API additionally returns `reference: {}`; the preconditions API
additionally returns `reason` equal to the input diagnostic. No partial decision
checks or reference selections accompany invalid input. If several inputs are
invalid, only the first detected failure is reported; that diagnostic can depend
on input order. Unexpected programming exceptions are not converted to verdicts.

The remaining public APIs extract, measure or validate data and raise
`InputInvalid` on validation failure. They do not return verdicts. In particular,
`same_point_record` constructs comparison data for later decisions.
`require_convergence_quantity(name)` returns `None` for `warpage_pv` or
`deformation_max` and raises `InputInvalid` for unsupported quantities. Each
decision API validates `warpage_pv` before calculating checks.

All accepted numbers are finite real values, excluding booleans. Dimensions are
actual integers, excluding booleans, with no string or float coercion. Numerical
calculations use binary64 without rounding before comparison. Thresholds are
inclusive and have no comparison tolerance. PV denominators must be positive.

## Decision result and check shapes

Every decision result contains `pass: bool` and `checks: list[dict]`.
`pass` is the conjunction of all check verdicts. Each check has exactly:

| Field | Type and meaning |
|---|---|
| `name` | String identifying the checked quantity and grid where applicable |
| `value` | Number, `(fvm, fem)` float tuple for curvature, `{same: float, nodal: float}` for reference convergence, or `None` when unavailable |
| `limit` | Numeric threshold, `"nonzero, same sign"` for curvature, or `None` when unavailable |
| `pass` | Boolean check verdict |
| `reason` | Empty string on success; nonempty diagnostic on failure |

`name` and `reason` are diagnostics, except that `name == "input"` identifies
validation failures. Failed numerical checks use the same shape. Missing required
gate records have `value` and `limit` set to `None`. An undefined convergence
order has `value: None` and a reason containing `阶不可判`.

### `fem_reference_self_convergence(tiers)`

Accept a nonempty list or tuple of tier dictionaries. Each tier requires positive
integer `nx, ny, kz`, positive `nodal_pv`, and a nonempty `same_point_pv` dictionary
mapping FVM `(nx, ny, kz)` tuples of positive integers to positive PV values.
FEM tiers are uniquely keyed by `(nx, ny, kz)`; duplicates fail without merging.
All five FEM keys `(12,4,2)`, `(12,4,4)`, `(24,8,2)`, `(48,16,2)` and `(96,32,2)`
are required, and all other FEM tier keys are rejected. Each required tier must
contain every FVM key present in any tier. Only the `(12,4,4)` thickness probe
uses kz4; all refinement candidates use kz2 and stop at `(96,32,2)`.
Unknown fields are ignored.

Return exactly `pass, checks, reference`. `reference` maps each converged FVM key
to its selected FEM key; unconverged keys are absent. Process FVM keys in ascending
tuple order, with thickness then reference checks for each key. Dictionary
insertion order follows that same FVM order. Results for valid records are
independent of tier and mapping insertion order.

Thickness compares `(12,4,2)` to `(12,4,4)` using
`abs(pv4-pv2)/pv4 <= 1e-3` for each FVM key. Reference selection scans adjacent
kz2 pairs in the prescribed ascending xy sequence. Select the finer key of the
first pair for which both same-point and nodal changes, divided by the finer PV,
are at most `1e-3`. The reference check reports that pair's two changes, or the
last pair's changes and `参考未收敛` if none qualifies. Thickness failure still
allows a reference selection, but makes the overall verdict fail.

### `scheme_a_gate(records)`

Accept a nonempty list or tuple of record dictionaries. Each requires positive
integer `nx, ny, kz`. kz16 records are diagnostics: after validating their key,
ignore them entirely. Other records require kz4 or kz8 and finite
`same_point_rel, deformation_rel, fvm_curvature, fem_curvature`. Their unique key
is `(nx, ny, kz)`; duplicates fail without merging. Valid strip dimensions are
`(48,16)` or `nx >= 96` with `ny*3 == nx`. Unknown fields, including interpolation
indicators, are ignored. Missing scalar fields fail input validation.

Return exactly `pass, checks`. Require both base groups `(48,16)` and `(96,32)`
and complete kz4/kz8 pairs for every supplied group. Missing records produce
failed checks. Order groups by `(nx,ny)`, then kz4 before kz8. For each record,
check absolute same-point error, absolute deformation error, then curvature;
follow each group with its thickness order. Finally check xy order between the
two base groups for kz4 then kz8. Valid record input order does not affect results.

| Check | `(48,16)` | All finer strip groups |
|---|---|---|
| kz4 absolute same-point error | `<= .030` | `<= .015` |
| kz8 absolute same-point error | `<= .010` | `<= .005` |
| Absolute deformation error | `<= .05` | `<= .05` |
| Curvature | Both nonzero and same sign | Both nonzero and same sign |
| Thickness order | `>= 1.5` | `>= 1.5` |

Order is `log2(abs(coarse)/abs(fine))`. Errors must be nonzero, have the same
sign and strictly decrease in magnitude. Thickness order additionally requires
`abs(coarse) > 1e-3`. Otherwise the order fails as undefined. The base-group xy
order must be at least `1.0` for each kz.

### `scheme_a_preconditions(nx, kz_min, same_point_rel, nodal_rel, legacy_gate)`

Require positive integer `nx` and `kz_min`, positive `legacy_gate`, and finite
signed relative errors. Only `nodal_rel=None` omits the nodal check. `nx` is
validated metadata; the caller supplies the threshold.

Return exactly `pass, checks, reason`. Check `kz_min >= 8`, then
`abs(same_point_rel) <= legacy_gate`, then `abs(nodal_rel) <= legacy_gate` if
present. `reason` joins failing check reasons with `"; "`, or is empty on success.

## Data and measurement APIs

| Function | Inputs and successful result |
|---|---|
| `plane_pv(xy, w)` | Finite numeric arrays shaped `(n,2)` and `(n,)`, `n>=3`; returns float peak-to-valley after shared least-squares plane removal. Zero is valid. |
| `top_face_centroids(run_dir, requested_nxny)` | Run directory path and two positive integer dimensions (tuple, list or array); returns binary64 array `(n,2)`, `n>=3`. See file rules below. |
| `same_point_record(fvm_run_dir, fem_run_dir, fem_same_point_pv)` | Run paths and callback `(fem_run_dir, xy) -> positive PV`; returns the six-field record below. |
| `deformation_max(points, displacement)` | Matching finite numeric `(n,3)` arrays, `n>0`; returns float maximum norm after shared infinitesimal rigid removal. Zero is valid. |
| `run_measured(argv, env, cwd)` | Subprocess argument sequence, environment mapping or `None`, working directory path or `None`; returns exactly `returncode: int, wall_s: float, peak_rss_mib: float`. |
| `rss_mib(ru_maxrss, platform)` | Nonnegative finite resource value and `linux` or `darwin`; returns float MiB using Linux KiB or Darwin bytes. |
| `mem_available_bytes()` | No arguments; returns nonnegative integer available bytes on Linux or Darwin. |

`same_point_record` returns exactly these fields, without interpolation indicators:

| Field | Meaning |
|---|---|
| `fvm_pv` | Positive FVM manifest PV |
| `fem_same_point_pv` | Positive callback PV |
| `same_point_rel` | Signed float `(fvm_pv-fem_same_point_pv)/fem_same_point_pv` |
| `fem_nodal_pv` | Positive FEM manifest nodal PV |
| `nodal_rel` | Signed float `fvm_pv/fem_nodal_pv-1` |
| `n_samples` | Integer number of reconstructed top samples |

The callback is called exactly once with the supplied FEM run path and
reconstructed xy after file/manifest validation. Callback exceptions and invalid
PV results become `InputInvalid`. No nodal fallback or default evaluator exists.

Run directories contain `manifest.json`. Sampling uses its first `steps` entry's
`file`; PV summaries match that entry's `n`, requiring exactly one `warpage_pv`
entry. FVM PV additionally requires the exact method
`FVM top-face centroids; least-squares plane removal`. `same_point_record` reads
requested `nx, ny` independently from FVM `case.json`'s `mesh` object.
The manifest mesh requires positive integer `nx0, ny0`, nonnegative integer `L`,
zero `padded_cells`, and `(nx0*2**L, ny0*2**L)` equal to the requested dimensions.

Sampling validates both `VTKHDF/hex` and `VTKHDF/cut` blocks: a single partition,
matching counts, bounded connectivity, nonempty monotone cell/face offsets and
consistent polyhedron face references. Empty blocks are allowed; an empty mesh
is invalid. Select the global top with strict `abs(z-z_top) < 1e-14`. Return hex
top-face vertex averages in cell order, followed by cut polygon area centroids
in face order. Preserve disconnected faces and reversed winding; do not sort or
merge samples. Require at least three samples and, for uncut strips, exactly
the refined `nx*ny` count. Validation finishes before returning samples.

Resource measurement supports Linux and Darwin, inherits output streams and
reaps exactly the launched child with `wait4`. Negative return codes represent
signals; nonzero child exit status is returned as data. Wall time is monotonic
elapsed seconds; peak RSS is raw per-child usage, without subtracting parent or
previous-child usage. Linux available memory uses only `MemAvailable`. Darwin
sums free, inactive and speculative pages and multiplies by `hw.pagesize`.
Unsupported platforms and invalid or unreadable resource data raise `InputInvalid`.
Interrupted `wait4` calls are retried. If measurement fails after launch, the
child is killed if still running and waited for before the error propagates,
including on `KeyboardInterrupt` or `SystemExit`. Successful measurements retain
the launched child's `wait4` usage; no cleanup result is returned as a measurement.
