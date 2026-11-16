# chipcae_material

Material-point library v0 for chipcae (namespace `chipcae::material`, C++20 + Kokkos,
FP64/SI only, Mandel notation). This repository directory is independently buildable.

## Requirements

- C++20 compiler (GCC 12+ tested with GCC 14)
- CMake >= 3.24, Ninja
- Kokkos (Serial + OpenMP for CPU) built with the same compiler
- Optional: Python 3.9+ and nanobind for the Python module

## Build and test (CPU)

```sh
cmake -S material -B material/build-cpu -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/workspace/deps/kokkos
cmake --build material/build-cpu -j
ctest --test-dir material/build-cpu --output-on-failure
```

Or with the helper script:

```sh
material/scripts/build_cpu.sh
material/scripts/run_all_acceptance.sh   # writes material/acceptance_report.json
```

If no Kokkos is installed, configure the library with the optional fetch switch instead:

```sh
cmake -S material -B material/build-cpu-fetch -G Ninja \
  -DCHIPCAE_MATERIAL_FETCH_KOKKOS=ON
```

The top-level chipcae build also works without Kokkos: `material/` prints a STATUS
message and is skipped.

## GPU

`material/scripts/build_cuda.sh` and `run_gpu_tests.sh` exist and exit non-zero with a
clear message when no CUDA toolkit / GPU is present. No GPU results are fabricated; the
CPU/GPU consistency item stays `pending`.

## Layout

- `include/chipcae/material/` — public headers (official v0.2.2 interfaces + helpers)
- `src/` — factory, schema validation, state serialization (host side)
- `driver/` — single-point strain/stress-controlled driver
- `tests/` — one executable per test, ctest label = test id
- `schema/` — JSON Schema and illustrative YAML examples
- `python/` — optional nanobind module
- `scripts/`, `ci/` — build, acceptance and CPU CI template

## Status

CHUNK 2 implements thermo-elastic and isotropic Prony point kernels, thermal
strain/CTE conversion, temperature tables, shift functions and reduced-time
integration. T0, T3 and focused `core_math` checks pass on CPU. Chemical shrinkage
and activation combinators remain for chunk 3; the full acceptance suite and GPU
validation remain pending. See `DELIVERY.md`.

Shrinkage binds through `chemical_shrinkage.schedule.cure_step_ids`, an array
with exactly one string id in P0. The schedule is later evaluated against
`ProcessClock.step_id` after the process layer resolves that id.
