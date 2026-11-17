# Sampledocs test-first evidence

Only the three new files under tests/unit/sample_docs are committed. The guide is intentionally absent; no production changes or stubs were needed. No numerical solve was launched by the new contract.

Build/test commands used plain tool names with PATH=/workspace/tasks/shim:$PATH (shim first):

```sh
cmake -S . -B /workspace/tasks/build-sampledocs-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build /workspace/tasks/build-sampledocs-release -j2
ctest --test-dir /workspace/tasks/build-sampledocs-release -R '^sampledocs_contract$' --output-on-failure
ctest --test-dir /workspace/tasks/build-sampledocs-release --output-on-failure
```

Configure and Release build: exit 0. Python ast.parse syntax check: PASS. git diff --cached --check: PASS.

Targeted CTest: exit 8, expected RED:

```text
Internal ctest changing into directory: /workspace/tasks/build-sampledocs-release
Test project /workspace/tasks/build-sampledocs-release
    Start 31: sampledocs_contract
1/1 Test #31: sampledocs_contract ..............***Failed    0.03 sec
sampledocs contract: missing guide: docs/examples/sample-readiness.md


0% tests passed, 1 tests failed out of 1

Label Time Summary:
tierA    =   0.03 sec*proc (1 test)

Total Test time (real) =   0.03 sec

The following tests FAILED:
	 31 - sampledocs_contract (Failed)
Errors while running CTest
```

The test was registered and Python executed successfully. Its first assertion reports the missing documentation file, matching the unimplemented feature rather than a compile/syntax error. Later assertions enforce exact S2 argv, read-only fixture references/table structure, and the required readiness concepts. The guide was not written to make this test green, as this is a TEST-FIRST-only pass.

Full CTest: exit 8; 31 tests, 4 failures, 1 skipped (26 passed). Besides sampledocs_contract, install_smoke, install_relocate and cpack_tgz fail because the default dependency-disabled configuration's preview reports:

```text
preview requires JSONCONS and HDF5 support
```

Full output: /workspace/tasks/logs/sampledocs-all-ctest.log. Configure/build logs: sampledocs-configure.log and sampledocs-build.log in this directory. Existing install failures are unrelated to the new Python test; no out-of-scope fixes were made. The all-pass requirement conflicts with the requested missing-feature RED state.

Manual source review: solver README requires single-knot E/nu/alpha and positive conductivity for steady conduction; FCBGA elastic and thermal_expansion temperature arrays contain two knots. thermal.case.json references that same material library, which lacks conductivity. The checkpoint report attributes original INPUT_INVALID/14 to temperature tables; its modified-input max_ratio error is a separate event. No guide exists yet to review.

Formatting: /home/box/.local/bin/clang-format --version reports 23.1.5. No C/C++ files were added or edited; clang-format -i and --dry-run --Werror have no applicable changed files (Python/CMake/empty marker only). Whitespace validation passed.
