# chipcae_material

C++20 + Kokkos material-point library: FP64, SI, Mandel notation, thermoelasticity,
isotropic Prony viscoelasticity, chemical shrinkage and transactional activation.
Both small strain and TotalLagrangian reference-strain kernels are supported.

Build independently from this repository root:

```sh
cmake -S material -B material/build-cpu -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$KOKKOS_ROOT" \
  -DCHIPCAE_MATERIAL_ENABLE_PYTHON=OFF
cmake --build material/build-cpu -j 4
OMP_NUM_THREADS=4 OMP_PROC_BIND=false material/scripts/run_all_acceptance.sh
```

The runner writes `material/acceptance_report.json` from measured CTest output.
T1–T23 and T24-pt pass on CPU; T0 and core_math also pass. T24 is skipped for later
FEM work. T25 and the 100-step CPU/GPU comparison remain pending. CUDA scripts
fail clearly without the toolkit; there are no GPU results.

Each acceptance ID has its own executable/target and CTest label. T14 invokes
SciPy's fixed-tau NNLS with `K3_TEST_PYTHON` set to a Python environment that
has NumPy and SciPy (defaults to `Python3_EXECUTABLE`). T24-pt's CTest label is
`T24pt` and its executable is `material_T24pt`.
T15 also runs independent 1- and 4-thread processes and compares complete output
bytes. See [DELIVERY.md](DELIVERY.md) for status, measurements and limitations,
and [DESIGN.md](DESIGN.md) for layouts and dispatch.

Chemical shrinkage requires post_gel_shrinkage.value and measure, plus exactly
one string in schedule.cure_step_ids. The process layer calls
`params.bind_cure_step(ordinal, start_seconds, end_seconds)` before make_material;
this resolves the string/clock ABI and actual process duration. Table profiles
use `schedule.fractions.{t_norm,fraction}`. gel_conversion and the obsolete
scalar cure_step_id are errors.

Factory models always reserve a 9-Real ActivationRecord. has_activation enables
the constitutive activation transform; has_shrinkage enables the eigenstrain
source. activate/deactivate edit trial. Evaluate from an immutable event snapshot
into trial, then commit or discard. The single-point driver's `context()` exposes
its ProcessClock. Save/restore accepts a POD byte stream with hash checks;
transfer_from fits affine fields inside the same material identity.

Native Python bindings are optional. The option defaults ON, but configuration
silently skips the module with a STATUS message when Python development headers
(including Python.h) or nanobind are unavailable. The current bindings.cpp is a
stub that only exposes `validate_material_json`; full driver, batch and CTE
bindings remain unfinished. These examples run without that module:

```sh
python3 material/python/examples/relaxation.py
python3 material/python/examples/cte_conversion.py
python3 material/python/examples/schema_error.py
python3 material/tools/dma_nnls.py
material/build-cpu/material_card_check material/tests/valid_elastic.json
```

Parameters in examples/tests are illustrative. The CLI accepts `--delivery` to
reject illustrative provenance. No new C++ dependencies were introduced.
