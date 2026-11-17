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
cmake --build <build-on-dir> -j2
cmake --build <build-off-dir> -j2
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
the goldens. STEP and ECAD-disabled tests compare defects and exit 7; STEP also freezes stdout/stderr.

CTest selects `import_cli_refactor_noop` in ECAD ON builds and
`import_cli_refactor_noop_ecad_off` in ECAD OFF builds; run both configurations.
Both register `import_cli_incomplete_reserved_unchanged`, `import_cli_step_unchanged`, `import_cli_reserved_args`, and
`import_cli_help_unchanged`. All carry the `tierA` and `import_cli` labels.

```sh
ctest --test-dir <build-on-dir> --output-on-failure
ctest --test-dir <build-off-dir> --output-on-failure
```

The generator records only unchanged baseline behavior (`noop`, `step`, `help`,
and `ecad_off`). Reserved options intentionally change diagnostics, so they have
no baseline goldens and are never passed to the generator. For each reserved
option on a valid input/output parse path, the test requires exit 2 and stderr
containing both `not implemented yet` and the option name. It also covers
`--t-min` placeholder values using the same substring contract. Conflicts in
both flag orders require exit 2 only. Missing reserved values on valid parse
paths require baseline help on stdout, empty stderr, and exit 2, including a
missing accept value after accept-b-tier. Reserved options consume their next
argument exactly like existing valued options, even when it resembles a flag.
Incomplete input/output invocations with supplied reserved values retain the
baseline unknown-argument response.
The planned `--assembly assembly.json -o out.stack.json` invocation is the explicit
exception and requires the reserved diagnostic in either option order.
`--assembly` determines the intentional diagnostic when output is present and input
is absent, including mixed reserved options in either order.
Output-only reserved invocations without assembly retain their baseline unknown-argument
response. Help paths are checked separately by the `help` case against the baseline help bytes.

Historically, before implementation the four reserved diagnostic assertions
failed because baseline stderr said `unknown argument`. The current test checks
the final contract; it is neither marked `WILL_FAIL` nor skipped.

Review regression coverage also compares the positive `--accept-b-tier` case,
`--out`, explicit `--polyset`/`--report` paths, and missing material-map output
against baseline goldens. The existing report format represents B-tier acceptance
as `"b_tier_accepted":"all"`. Help goldens cover `-h`, missing input/output,
and a missing `-o` value; mixed `a.xml -o b --help` must report an unknown argument.
Each new case compares stdout and stderr as well as exit status and artifacts.

The additional goldens were generated from a clean detached worktree at the same
baseline SHA, configured with ECAD ON, tests OFF, and Release, then built with
`cmake --build <baseline-build-dir> --target chipcae-import -j2`. Run the `noop`
and `help` cases with `--generate` to reproduce them. The measured toolchain was
GNU C++ 14.2.0 on Linux. Byte-exact floating-point text can depend on the compiler
and standard library; goldens intentionally remain byte comparisons.

Duplicate positional input, output, material-map, and accept-b-tier cases compare
exit status, stdout, stderr, and artifacts with the same main-baseline binary.
Missing polyset/report/material-map values also compare baseline help bytes.
The `.chipcae-autotests` marker enables `chipcae_auto_test_subdirs()` discovery
from tests/CMakeLists.txt; this directory's CMakeLists.txt registers these regression tests.

Empty and truncated XML fixtures also compare exit status, stdout, stderr, and
all artifact bytes against the same baseline in the `noop` case. Each is copied
to a relative `input.xml` path so diagnostics do not depend on the checkout path.

Review follow-up: conflict handling deliberately returns exit 2 with empty stdout
and stderr in both flag orders; tests now freeze that silent-output contract.
Incomplete supplied-value reserved invocations are registered separately as
`import_cli_incomplete_reserved_unchanged` and compare exit/stdout/stderr against
committed main-baseline goldens, rather than hand-written diagnostics. Assembly
with output remains in the intentional-change tests. Existing boundary assertions
remain in place so coverage is not reduced.

Additional stdout/stderr goldens for the four boards and STEP, mixed-help goldens,
and incomplete-reserved goldens were generated with the existing Release binary
at `/workspace/tasks/istepa-golden-build/kernel/chipcae-import` from the detached
`1607ad5fde6641a92fb7da7a0a66f456e4ec4adf` baseline source. Reproduce using the
`noop`, `step`, `help`, and `incomplete_reserved` cases with `--generate`.
ECAD OFF retains the brief's exact exit-code/defects comparison scope.
