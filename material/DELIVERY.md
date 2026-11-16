# DELIVERY — chipcae_material v0, chunk 2

## Build and validation

```sh
cmake -S material -B material/build-cpu -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/workspace/deps/kokkos
cmake --build material/build-cpu -j 4
OMP_NUM_THREADS=4 ctest --test-dir material/build-cpu --output-on-failure
```

Kokkos 4.7.1 Serial/OpenMP, FP64. All three registered tests pass: T0, T3,
and `core_math`. Python keeps its graceful disabled fallback because development
headers are unavailable. Changed C++ is clang-formatted. Parent CMake and CI
workflows were not changed.

| ID | Status | Notes |
| --- | --- | --- |
| T0 | pass | factory, driver, Kokkos and state IO smoke |
| T3 | pass | existing Mandel tests retained |
| T1, T2, T4–T23 | later chunks | full acceptance suite not started; focused math checks are not full acceptance |
| T24 | skip | later FEM work with K2/P1 |
| T24-pt | later chunk | point-level substitute still required |
| T25 / CPU–GPU consistency | pending | no GPU/toolkit available; no GPU results claimed |

`core_math` checks exact CTE integrals including jumps and stress-free overrides,
node and off-knot CTE conversion, elastic secant response and finite-difference
tangent, TI/orthotropic rotations, shift normalization on both sides of Tg,
quadrature against a host E1 primitive, reversed ramps, shift-table knot splits,
both Prony modes at zero/finite/large reduced time, dissipation, HostSpace kernels,
and two-point batch initialization/state agreement. Parser checks include P5,
duplicate modulus knots, missing volumetric mode and approximation metadata.
Cases are deterministic (no random seed).

## Implemented

- Immutable temperature/Prony snapshots in read-only Kokkos Views, copied into
  kernel closures; all point update/init methods are device-callable const methods.
- Elastic local compliance, Cholesky inversion, rotation and full current-strain
  stress for isotropic, transversely isotropic and orthotropic symmetry. Both
  kinematics share this map; no push-forward is done.
- Exact integrated tangent CTE and direct secant CTE thermal strain, explicit
  stress-free override, CTE type/reference/stress-free metadata.
- WLF, Arrhenius, normalized piecewise shift and table shift. Reduced time uses
  adaptive three-point Gauss with explicit bounded stack (20 levels), splitting
  at Tg and table knots. Host E1 reference available for physical positive
  coefficients; table reference delegates to quadrature.
- Both Prony volume modes, local-frame history, consistent tangent, stable g(x),
  zero-dt and large-x limits, trapezoidal dissipation and approximation note.
- Handwritten JSON subset reader, parameter extraction and initial semantic checks.
  Modulus duplicates, missing CTE convention/secant reference, missing volume
  mode, orthotropic viscoelasticity and invalid P0 binding lists are rejected.
- Fixed pre-existing batch initialization component stride and empty-state pointer
  handling; forwarded constant-Poisson approximation metadata through IMaterial.
- Updated schema/example/API comments to `cure_step_ids` with exactly one item.

## Deviations and remaining scope

- Official point update signatures are unchanged. Additional parameter constructors,
  TableView and exact scalar CTE conversion overloads make the math usable.
  ShiftFunction no longer owns a host std::vector parameter object (or exposes its
  old reference accessor); keeping that object in a captured kernel violates the
  device rules. ShiftParams remains the host input structure.
- Parameter snapshots currently use explicit DefaultExecutionSpace::memory_space.
  CPU HostSpace is verified; independently selectable host/device parameter
  spaces and GPU factory instantiations require later integration and validation.
- Node-sampled converted CTE tables generally cannot reproduce the exact converted
  function under linear interpolation. Scalar conversion overloads preserve the
  underlying formulas at arbitrary temperatures. At interior derivative knots,
  the right-hand slope is used; tangent-to-secant uses constant endpoint extension.
- JSON reader intentionally supports a subset (no Unicode escape decoding).
  Complete schema/provenance validation, positive-definiteness sampling between
  elastic knots, duplicate CTE knot checks specifically at Tg, structured warning
  aggregation, and the full T16 fixtures are later work. Host TemperatureTable
  exposes a clamp warning flag; device snapshots do not aggregate host warnings.
- Parameter hash is deterministic FNV-1a of input JSON bytes, not canonical JSON.
  Prony layout has field metadata and a version/count/mode hash; full checkpoint
  parameter-hash integration remains later work.
- Chemical shrinkage schedule/source/combinator and activation remain stubs for
  chunk 3. Parsed shrinkage binding ids are validated but are not yet applied by
  the base-model factory. State transfer still has the skeleton fallback. Python
  polish, DMA fitting, full acceptance reporting and GPU testing remain later work.
- No rebase, push, PR, dependency additions, cloud work or repo settings changes.
