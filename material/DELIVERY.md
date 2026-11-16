# K3 material v0 delivery — chunk 3

Implemented locally on `k3/material-v0`, based on the supplied rebased branch.
No push, PR, workflow/settings change, cloud work, or new C++ dependency.

## Build and reproduce

```sh
cmake -S material -B material/build-cpu -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/workspace/deps/kokkos \
  -DCHIPCAE_MATERIAL_ENABLE_PYTHON=OFF
cmake --build material/build-cpu -j 4
BUILD_JOBS=4 OMP_NUM_THREADS=4 OMP_PROC_BIND=false \
  material/scripts/run_all_acceptance.sh
```

GCC 14, Kokkos 4.7.1 Serial/OpenMP, FP64, no fast-math or altered FMA settings.
The runner executes all CPU checks and extracts actual numerical metrics from
CTest JUnit into `acceptance_report.json`. Report entries with multiple criteria
retain the error/tolerance pair with the highest utilization; boolean/bitwise
checks use 0 error and 0 tolerance. T8 reports the absolute deviation of observed
order from 2 (limit 0.2), rather than labelling an order as an error. Seeds are
20261116. T0 and core_math remain additional regression checks.

CPU result: **24 acceptance IDs pass, 0 fail; T24 skip; 2 GPU entries pending.**
CTest: 27 passing checks (including T0, core_math and the extra T15 thread
comparison), plus the explicitly skipped T24/T25 entries.

| ID | Status | Measured error | Tolerance |
| --- | --- | ---: | ---: |
| T1 | pass | 2.68062e-10 | 1e-06 |
| T2 | pass | 0 | 1e-13 |
| T3 | pass | 6.10623e-15 | 1e-14 |
| T4 | pass | 2.50541e-15 | 1e-12 |
| T5 | pass | 9.70292e-16 | 1e-12 |
| T6 | pass | 2.5346e-16 | 1e-12 |
| T7 | pass | 2.69525e-16 | 1e-12 |
| T8 | pass | 0.000622988 | 0.2 |
| T9 | pass | 0 | 1e-12 |
| T10 | pass | 0 | 0 |
| T11 | pass | 2.38761e-15 | 1e-12 |
| T12 | pass | 1.01035e-14 | 1e-13 |
| T13 | pass | 0 | 1e-14 |
| T14 | pass | 2.13174e-16 | 1e-06 |
| T15 | pass | 0 | 0 |
| T16 | pass | 0 | 0 |
| T17 | pass | 6.24964e-16 | 1e-12 |
| T18 | pass | 5.02858e-16 | 1e-12 |
| T19 | pass | 7.29668e-08 | 1.11111e-07 |
| T20 | pass | 7.84813e-13 | 1e-10 |
| T21 | pass | 2.32831e-16 | 1e-12 |
| T22 | pass | 1.3769e-12 | 1e-08 |
| T23 | pass | 4.35401e-16 | 1e-12 |
| T24 | skip | — | — |
| T25 | pending | — | 1e-12 |
| T24-pt | pass | 5.4186e-16 | 1e-12 |
| CPU/GPU-100-step | pending | — | 1e-11 |

## Implemented behavior

- Linear/table equivalent shrinkage, linear-to-volume conversion, step-local
  scheduling, stateful hold, unchanged consistent tangent, chemical strain output,
  and generic heterogeneous eigenstrain composition. CureKineticsShrinkage is
  intentionally only declared.
- Always-present activation prefix, default active=1, transactional activation and
  deactivation, reset history/source state, strain and thermal references,
  Excluded and parameterized SoftStiffness modes.
- POD checkpoint/restart including FNV-1a hashes, endianness/version/numbering,
  same-device bitwise replay and discard, O(1) commit, and affine 3D local
  least-squares transfer of transferable fields within a material identity.
- Local Newton stress control and strain control. Factory dispatch includes both
  ModelKind values and all four shrinkage/activation flag combinations.
- Schema rejection fixtures, positive-definiteness sampling at knots and eight
  interior temperatures, strictly increasing Prony times, required CTE/volume
  conventions, shrinkage table semantics and customer-delivery provenance checks.
  Load temperatures outside tables return OutOfRange; the batch adapter enforces
  fit.valid_temperature_range and exposes a host clamp warning when selected.
- Fixed-tau SciPy Lawson–Hanson NNLS and runnable no-module relaxation, CTE and
  native schema CLI examples. No full DMA import/master-curve application.

## Defaults and interface decisions

| Decision | Applied default |
| --- | --- |
| D4 | Two ModelKind values × shrinkage flag × activation flag; eight adapter instances |
| D5 | All factory layouts begin with 9 transferable activation Reals; active=1 |
| D6 | Linear tables only; pchip and constant rejected during validation |
| D7 | Shift-table log10(aT) linear in T; integration splits at knots |
| D8 | Iterative Gauss-3, maximum subdivision depth 20, failure returns CutStep |
| D9 | SoftStiffness factor 1e-6, positive finite constructor parameter |
| D10 | Minimal fixed-tau scipy.optimize.nnls only |
| D11 | Caller supplies k≥4 indices and 3D coordinates; affine least squares; rank failure is an error |
| D12 | POD committed-state byte stream; no HDF5 |
| D13 | FNV-1a 64-bit; canonical little-endian layout metadata; raw JSON byte parameter hash |
| D14 | volumetric_mode required, no default in cards |
| D15 | Handwritten MaterialParams and JSON subset parser |
| D16 | T1 samples avoid Tg ±1e-3 K and modulus-ramp endpoints; no tolerance relaxation |

The official update signatures are retained. Additions needed to implement
composition/checkpoint semantics are documented here: PointInput carries an
optional thermal-zero override; IMaterial has default material_id(), parameter_hash() and
clamped_warning() metadata accessors; MaterialAdapter accepts parameter identity
and fit bounds; the store constructor accepts parameter hash, material identity
and numbering scheme. These defaults preserve existing callers. WithActivation's
optional template boolean reserves the prefix without enabling transforms.
Schema validation has an optional customer_delivery argument. The driver exposes
context() so callers can supply ProcessClock for stress-controlled cure steps.

The updated brief's **cure_step_ids list, exactly one item**, overrides older
scalar cure_step_id excerpts; the scalar is explicitly rejected. The other
shrinkage conflict (β·Δp versus s_eff) follows the brief's P0 s_eff decision.
The official ProcessClock uses a numeric id while cards use strings and do not
carry process duration. The process layer must call
`params.bind_cure_step(ordinal, start_seconds, end_seconds)` before constructing
the material. This binding extends the parameter hash. Optional process_step_ids
in a combined JSON input validates names and maps them to ordered zero-based
ordinals. Standalone unbound cards use process_step_id(name), an opaque FNV id,
and retain s_applied outside that id; an unbound schedule's illustrative interval
is [0,1] seconds. Do not use that fallback as a recipe-duration inference.
For ordered bound schedules, times before the selected step give zero and times
after it hold the full effective shrinkage, including a skipped endpoint.

Activation is evaluated at the process event time (first evaluation at the start
of the new step, then subsequent increments). Solver code evaluates constitutive
trial from an immutable snapshot of the event-edited trial; it must not overwrite
committed history while iterating. Activating the same material repeatedly at an
arbitrary time inside an already-running cure schedule requires a process-layer
rebinding of the remaining cure schedule.

## D19 number parsing

Schema number access coerces JSON numeric values and strictly validated quoted
decimal/scientific strings to finite FP64 values. Invalid strings fail with their
material field path. The Python YAML helper installs a PyYAML float resolver for
scientific forms such as `3.10e10`; C++ and Python tests cover the requested
equivalent spellings and trailing-junk rejection. Bundled material YAML cards use
canonical positive exponent spelling (`e+`).

## Environmental limits, deviations and unfinished work

- **GPU: scripts ready, awaiting cloud GPU.** No CUDA toolkit or GPU is available.
  Both CUDA scripts were invoked and returned 2 with the explicit nvcc-missing
  diagnostic. T25 and CPU/GPU-100-step remain pending, never counted as failures.
  CUDA compilation, independent host/device parameter placement and device factory
  instantiation are unvalidated; the current point suite is CPU only. The T25
  CTest entry is explicitly skipped, not a GPU result. A real device consistency
  suite must replace it when that integration is undertaken.
- T24 is skipped for later K2/P1 FEM convergence work. T24pt tests constrained
  material stresses and the Timoshenko force/moment release formula to 1e-12;
  it does not claim structural mesh convergence.
- Python.h is unavailable. Nanobind remains optional/disabled. The existing module
  only exposes validation; native driver/batch/CTE bindings remain unfinished.
  Three runnable examples and the minimal NNLS tool do not need that module.
- JSON is a documented subset (no Unicode escapes); YAML may be converted with
  Python. Full JSON-Schema code generation, unknown-field rejection, automatic
  duplicate-modulus ramp import, and general import tooling are not implemented.
  Repeated modulus temperatures are rejected; the tested 1 K ramp is explicit.
- Host TemperatureTable and fit-range clamping expose warnings. General
  device-table clamp-warning aggregation and application logging of activation
  thermal-reference changes are left to the host integration layer.
- Schedule tables are capped at 256 knots; compositors at 64 StateFields / 65535
  Reals. Larger inputs fail explicitly. Parameter hashing is byte-based, not
  canonical JSON; process binding should be performed once before factory use.
- Exact scalar CTE conversion is authoritative. Interpolating only converted
  node samples generally loses the underlying function between knots; tests use
  a representable case for kernel equivalence and exact off-knot conversions.
- ∂stress/∂T, full cure kinetics, orthotropic viscoelasticity and solver/FE
  assembly remain intentionally outside v0. No missing CPU implementation is
  being excused as a GPU or Python environmental limitation.

`chipcae_material.tar.gz` packages material sources, examples, documentation and
acceptance_report.json, excluding local builds and Python caches.
