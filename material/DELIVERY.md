# DELIVERY — chipcae_material v0 (CHUNK 1 skeleton)

## Build

```sh
cmake -S material -B material/build-cpu -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/workspace/deps/kokkos
cmake --build material/build-cpu -j
ctest --test-dir material/build-cpu --output-on-failure
```

## Test status

| ID | Status | Notes |
| --- | --- | --- |
| T0 (smoke) | pass | compiles, initialises Kokkos, links library, factory works |
| T3 | pass | Mandel inner product, Voigt↔Mandel round-trip, Q_M orthogonality/det |
| T1, T2, T4–T23 | later chunks | declared `TODO(K3)` stubs returning NotImplemented |
| T24 | skip | structural FEM work, out of K3 scope |
| T24-pt | later chunk | point-level substitute |
| T25 | pending | no GPU available; scripts ready |

## Deviations / conflicts

- Official headers were copied and only given `#pragma once`, includes and
  out-of-line/template implementations; no declaration semantics changed.
- `MaterialAdapter` uses `Kokkos::DefaultExecutionSpace` (or
  `DefaultHostExecutionSpace` for `HostSpace`) to launch kernels; a direct
  `RangePolicy<MS>` would treat a memory space as an execution space.
- Schema `id` pattern was corrected to `^[a-z0-9_-]+$`; `volumetric_mode` is required
  with no default (D14).
- Parameter hash is written as 0 until the host-side parameter hashing lands (D13).

## Not implemented in CHUNK 1

- Constitutive kernels (thermo-elastic, Prony), thermal strain integration, shift
  functions, chemical shrinkage, activation combinator semantics.
- Schema JSON parser/semantic validator (structural check only), full save/restore
  parameter hash, `transfer_from` least squares (nearest-neighbour fallback).
