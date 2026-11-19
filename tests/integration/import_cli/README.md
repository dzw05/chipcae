# Import CLI acceptance tests

Historical golden source: `1607ad5fde6641a92fb7da7a0a66f456e4ec4adf`, the 11/17
`origin/main` revision identified in the brief. The test-first branch HEAD was
exactly this revision when the Release binaries were built; only tests and their
registration had been added, and no production sources were changed.

Following the 2026-11-19 03:55 supplemental ruling for #139, material id
strings in the ten scoped `stack.json` goldens were migrated by exact text
replacement: `"mat.fr4"` to `"fr4_eq"` and `"mat.cu"` to `"cu"`. All other
bytes remained the output of baseline `1607ad5`, including the polyset files and
their SHA-256 references, until the I1 1.1 update below. The updated
`material_map.json` with `FR4` mapped to `fr4_eq`, `copper` mapped to `cu`,
and the global fallback `copper_base` mapped to `fr4_eq` is required for current tests.

Following I1 1.1, Patterned layer `material` now identifies the base dielectric;
`copper.metal` remains the metal ID. These goldens also lock the importer to
polygon output without `copper.fraction`.
Only Patterned `material` values changed from `cu` to `fr4_eq`; all other bytes,
including polysets and SHA-256 references, were preserved.
The synthetic `schema/examples/copper_polygons.stack.json` example uses a
`CCPOLY01` binary sidecar named `copper_polygons.polyset.json`; despite its
extension, the sidecar is not JSON text.

Historical baseline generation commands, run from the root of a separate worktree
at `1607ad5` (replace the build-directory placeholders with local paths):

```sh
cmake -S . -B <build-on-dir> -G Ninja -DCMAKE_BUILD_TYPE=Release -DCHIPCAE_ENABLE_ECAD=ON -DCHIPCAE_BUILD_TESTS=ON
cmake -S . -B <build-off-dir> -G Ninja -DCMAKE_BUILD_TYPE=Release -DCHIPCAE_ENABLE_ECAD=OFF -DCHIPCAE_BUILD_TESTS=ON
cmake --build <build-on-dir> -j2
cmake --build <build-off-dir> -j2
tests/integration/import_cli/gen_golden.sh <build-on-dir>/kernel/chipcae-import <build-off-dir>/kernel/chipcae-import
```

These commands reproduce the historical baseline, not the current goldens.
Do not regenerate the committed goldens with `gen_golden.sh`: the baseline binary
reuses the copper metal ID for Patterned `material`, even with the updated map.
To reconstruct the current goldens independently, generate the baseline artifacts
in a separate worktree, apply the material ID migration above, and then replace
only Patterned `material: cu` values with `fr4_eq` in the scoped `stack.json` files.
Preserve every other byte. Do not generate from the implementation under test.
CI only reads the committed goldens and never builds an old binary.
The fixed material map is `material_map.json`; inputs are the four existing XML
fixtures in `../ecad/data/`. `input.step` is a minimal synthetic STEP placeholder:
the baseline rejects STEP before reading it.

Each invocation uses a fresh working directory and relative output `stack.json`.
This preserves the baseline's `stack.polyset` reference without rewriting any
output bytes. Tests compare numeric exit codes and the five named artifacts
(`stack.json`, `stack.polyset`, `defects.json`, `import_report.json`, and
`import_report.csv`), including whether each artifact exists. Unchanged cases use
byte comparisons without sorting or normalization. The unsupported IPC fixture
now requires exit 6, no geometry/layer artifacts, and complete B defects in JSON
and CSV; its old successful-import golden is retained only as the unchanged
report-header baseline. Help checks the original usage bytes plus the exact ECAD
help extension. Never format the goldens. STEP and ECAD-disabled tests compare
defects and exit 7; STEP also freezes stdout/stderr.

CTest selects `import_cli_refactor_noop` in ECAD ON builds and
`import_cli_refactor_noop_ecad_off` in ECAD OFF builds; run both configurations.
Both register `import_cli_incomplete_reserved_unchanged`, `import_cli_step_unchanged`, `import_cli_reserved_args`, and
`import_cli_help_unchanged`. All carry the `tierA` and `import_cli` labels.

```sh
ctest --test-dir <build-on-dir> --output-on-failure
ctest --test-dir <build-off-dir> --output-on-failure
```

The generator records only unchanged baseline behavior (`noop`, `step`, `help`,
and `ecad_off`, plus `incomplete_reserved`). Supplied reserved options on normal
parse paths intentionally change diagnostics and are not passed to the generator. For each reserved
option on a valid input/output parse path, the test requires exit 2 and stderr
containing both `not implemented yet` and the option name. It also covers
`--t-min` placeholder values using the same substring contract. Conflicts in
both flag orders require exit 2 only. Missing reserved values on valid parse
paths require baseline unknown-argument stderr, empty stdout, and exit 2, including a
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
An explicit `add_subdirectory(integration/import_cli)` in `tests/CMakeLists.txt`
registers these tests before the auto-scan, as permitted by the review follow-up.
The auto-scan skips explicitly added directories, preventing duplicate inclusion
even if an auto-discovery marker is added later.

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

Mixed reserved assembly regressions exercise all six orderings of the output,
reserved option, and assembly pairs, with both `-o` and `--out`. Each requires
the assembly diagnostic, exit 2, and empty stdout.

Missing reserved values and repeated missing accept values are compared byte-for-byte
with goldens from the same baseline binary via `incomplete_reserved --generate`.

Error-precedence regressions in `incomplete_reserved` compare 28 additional
invocations against the same baseline binary: duplicate input before and after
output, unknown options, and mixed help preceding each reserved option; plus
all ordered pairs of accept/t-min/stack-override on output-only paths with both
output aliases. Exit status, stdout, stderr, and artifact presence/bytes are
compared without normalization. Reproduce with `incomplete_reserved --generate`
using the documented baseline binary. Baseline rejects the first unknown reserved
token without consuming its value; for example output-only `--accept x
--stack-override y` reports `unknown argument: --accept`.
