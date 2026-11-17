Synthetic STEP fixture setup (OCCT builds only):

```sh
cmake -G Ninja -S tests/integration/import_assembly/fixtures \
  -B <main-build>/sp4-step-fixtures -DCMAKE_BUILD_TYPE=Release \
  -DOpenCASCADE_DIR=<same OpenCASCADE_DIR as main-build>
cmake --build <main-build>/sp4-step-fixtures -j2
```

Run these before CTest so its 60-second test timeout cannot terminate a build
waiting for a shared slot. Alternatively set `SP4_FIXTURE_GENERATOR` to an
already-built `sp4_step_fixtures` executable. Each test generates STEP files
in its own temporary directory. No vendor geometry is stored here.

The C++ tests use private orchestration helpers `map_step_defects`,
`assembly_error_exit`, and `inline_assembly_outline` in the importer namespace.
Until SP4 supplies them, test-local fallbacks return empty results, exit zero,
and an unchanged outline, respectively. The writer fallback returns an error.
These deliberately fail behavioral assertions and never alter the CLI.
