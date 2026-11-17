# Sample documentation acceptance evidence

The original test-first run is preserved in
[sampledocs-initial-contract.log](sampledocs-initial-contract.log), copied from
`/workspace/tasks/logs/sampledocs-contract.log`. The contemporaneous
[test-first report](sampledocs-test-first.md) records the command and exit 8.
These are original local artifacts, not a newly simulated historical run.

The repository independently establishes the order:

```sh
git show 665f332bc1583a347268cb9b292387e407a2434c:tests/unit/sample_docs/test_contract.py
git ls-tree -r --name-only 665f332bc1583a347268cb9b292387e407a2434c docs/examples/sample-readiness.md
git log --reverse --format='%H %s' 665f332bc1583a347268cb9b292387e407a2434c^..51380b9d2845c2de73597ac716a6555c7023a139
```

The test commit contains the contract but no guide; its immediate child
`51380b9d2845c2de73597ac716a6555c7023a139` adds the guide. The recorded first
CTest run fails with `missing guide: docs/examples/sample-readiness.md`.
The original report also records unrelated packaging failures; it is retained
unaltered and does not claim a full-suite pass.

The review fix parses the profile and case using published schemas, resolves
stack/material references relative to the case, validates their schemas and
material mappings, and checks S2 single-knot E, nu and alpha values.
The run recorded in [sampledocs-input-mutations.log](sampledocs-input-mutations.log)
used temporary fixture copies: invalid ranks, missing stack/material references,
empty layers, unknown material IDs, missing E and multiple temperature knots
were each rejected; restored inputs passed. No numerical solver was run.

## Final Release verification

Run from `/workspace/chipcae-sampledocs` with the shared build wrappers first
in PATH, using plain command names:

```sh
export PATH=/workspace/tasks/shim:$PATH
cmake -S . -B /workspace/tasks/build-sampledocs-deps-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCHIPCAE_ENABLE_JSONCONS=ON -DCHIPCAE_ENABLE_HDF5=ON '-DCMAKE_PREFIX_PATH=/workspace/deps/jsoncons;/workspace/hdf5-serial' -DHDF5_ROOT=/workspace/hdf5-serial
cmake --build /workspace/tasks/build-sampledocs-deps-release -j2
ctest --test-dir /workspace/tasks/build-sampledocs-deps-release --output-on-failure
```

Configure, build and final CTest each exited 0. The complete final CTest output
is [sampledocs-final-ctest.log](sampledocs-final-ctest.log): 42 tests registered,
38 passed, four skipped, zero failed. `sampledocs_contract` passed. The existing
skip behavior remains intact: `cut_results_serial`, `contract_output_solver`,
`install_smoke` and `cpack_tgz` require unavailable numerical backend features.
No tests were disabled or given new skip conditions.

The unsupported historical solver-result requirement was replaced by a check
of the checked-in temperature-table limitation, matching the corrected guide;
input validation was strengthened. All other documentation assertions remain.

`/home/box/.local/bin/clang-format` reports 23.1.5. Its `-i` and subsequent
`--dry-run --Werror` commands on `tests/unit/sample_docs/CMakeLists.txt` exited 0
without changes. No C/C++ source files changed. `git diff --check` passed.
