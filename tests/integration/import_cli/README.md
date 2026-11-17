# Import CLI acceptance tests

Golden source: `1607ad5fde6641a92fb7da7a0a66f456e4ec4adf`, the 11/17
`origin/main` revision identified in the brief. The test-first branch HEAD was
exactly this revision when the Release binaries were built; only tests and their
registration had been added, and no production sources were changed.

Generation commands, run from the repository root (replace the build-directory
placeholders with local paths):

```sh
cmake -S . -B <build-on-dir> -G Ninja -DCMAKE_BUILD_TYPE=Release -DCHIPCAE_ENABLE_ECAD=ON -DCHIPCAE_BUILD_TESTS=ON
cmake -S . -B <build-off-dir> -G Ninja -DCMAKE_BUILD_TYPE=Release -DCHIPCAE_ENABLE_ECAD=OFF -DCHIPCAE_BUILD_TESTS=ON
cmake --build <build-on-dir> -j3
cmake --build <build-off-dir> -j3
tests/integration/import_cli/gen_golden.sh <build-on-dir>/kernel/chipcae-import <build-off-dir>/kernel/chipcae-import
```

To regenerate later, build that source revision in a separate worktree and pass
its two binaries to `gen_golden.sh`. Do not generate from the implementation
under test. CI only reads the committed goldens and never builds an old binary.
The fixed material map is `material_map.json`; inputs are the four existing XML
fixtures in `../ecad/data/`. `input.step` is a minimal synthetic STEP placeholder:
the baseline rejects STEP before reading it.

Each invocation uses a fresh working directory and relative output `stack.json`.
This preserves the baseline's `stack.polyset` reference without rewriting any
output bytes. Tests compare numeric exit codes and the five named artifacts
(`stack.json`, `stack.polyset`, `defects.json`, `import_report.json`, and
`import_report.csv`), including whether each artifact exists. There is no sorting,
JSON parsing, normalization, or comparison of unrelated artifacts. Never format
the goldens. STEP and ECAD-disabled tests compare only defects and exit 7.

CTest selects `import_cli_refactor_noop` in ECAD ON builds and
`import_cli_refactor_noop_ecad_off` in ECAD OFF builds; run both configurations.
Both register `import_cli_step_unchanged`, `import_cli_reserved_args`, and
`import_cli_help_unchanged`. All carry the `tierA` and `import_cli` labels.

```sh
ctest --test-dir <build-on-dir> --output-on-failure
ctest --test-dir <build-off-dir> --output-on-failure
```

Before implementation, all regression guards must pass. Only the four reserved
option diagnostics are expected to fail: baseline stderr says `unknown argument`
rather than `not implemented yet`. The reserved test collects all four failures
and checks each option name and exit 2. It also checks conflict exit 2 in both
flag orders, without requiring particular conflict text. These are deliberate
failing assertions, not tests marked `WILL_FAIL` or skipped.
