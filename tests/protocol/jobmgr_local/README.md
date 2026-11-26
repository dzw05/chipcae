These acceptance tests cover both cards of I7-jobs-local/1.0. They use explicit
`require` checks in Release builds. `test_paths` always runs; the other manager
tests skip with code 77 only when the optional local target is absent.

The launch card exercises the default POSIX launcher and blocking `wait` with
real child processes. Existing state tests inject FakeSpawner explicitly. Launch
and cancellation tests use ordinary failure semantics; dependency absence alone
causes return-77 skips.

`test_launch_e2e` uses the production EventLogWriter in a standalone mock solver
for success reasons, missing/torn/invalid/failed/no exit logs, exit 9, signal 139,
launch errors, and fake MPI. It compares returned/Get/persisted jobs, captured
argv, PID, OMP overrides, inherited environment and working directory, including
spaced/UTF-8 paths and executable PATH lookup. Three simultaneous children across
two managers check that reaping neither steals another owner's exit nor returns
an exit twice. It never runs a real solver or real mpirun.

`test_cancel` checks TERM, grace-expiry KILL independent of polling, zero grace,
reaping, sticky CANCELLED state, manager destruction and recovery, and wait
boundaries, including a positive timeout with FakeSpawner to isolate polling
from process startup. The Linux `/proc` zombie check waits for mock readiness
published after installing SIGTERM handling.
Emergency test cleanup runs only after the ownership assertions and manager
destruction. The existing `test_calls` retains
the recorded 120-second to 60000-ms clamp test without waiting a minute.

`test_desktop_parity` compiles the original desktop ProfileMapper.cpp and
JobTypes.cpp directly with the same generated job protocol. It compares mapped
protobuf bytes, each UTF-8 argument, the exact environment override, six invalid
profiles, and both directions of QLockFile exclusion. It covers direct/MPI,
default/explicit threads, CPU/available CUDA, and spaced paths. Without Qt Core
it prints exactly `SKIP: desktop Qt sources unavailable` and returns 77. Without
protobuf/jsoncons all three new tests compile only their stub mains and skip;
mock_solver is not built. Path/core checks keep ordinary failure semantics.

Local validation uses a Release configuration with protobuf, jsoncons and Qt
Core, then a protobuf-disabled build and a Qt-disabled parity build. Main CI
usually lacks protobuf/Qt, so its dependency skips do not establish launch or
desktop parity coverage; local results must be reported as 实测. The external
implementation evidence log records commands, red/green results, dependency skips,
and full-suite baseline comparisons.

Recovery makes every formerly live job terminal. New Control calls after
restart therefore reject; cached accepted replies retain their original uint64
sequence (including values beyond double precision). The malformed label
selector check also applies to an empty root, stricter than desktop validation
inside iteration.

`test_snapshot_stream` covers incremental SHA-256 standard vectors and every
split of seeded inputs of length 0–200, stream-copy/hash-only chunk boundaries,
JSON prefixes across chunks, atomic mismatch/I/O cleanup, and recursive Submit
byte/hash equivalence. BOM/comment references, duplicate/cyclic normalized
paths, conflicting hashes, and unchanged duplicate-target sentinels protect the
existing snapshot contract. Only the chmod-0555 subcase skips when running as
root; missing LocalJobManager returns 77 for the whole executable.

The CLI memory regression generates synthetic binary attachments incrementally
with a fixed seed and leading NUL. `posix_spawn` plus EINTR-aware `wait4` measures
child RSS without `/proc`; Darwin bytes are converted to KiB. Each of 1 MiB and
48 MiB runs three times, verifies successful Submit, compares output in fixed
blocks, and checks manifest hashes against independently computed fixture
hashes. The minimum-RSS difference must be at most 4096 KiB.

TEST-FIRST measured result (Release, before adding private-interface stubs):
1 MiB RSS was 16576/16508/16588 KiB; 48 MiB RSS was
160976/160840/160900 KiB. The 144332 KiB minimum-RSS difference fails the
4096 KiB assertion after all six successful byte/hash integrity checks.
The extended layout test passes with source-derived fixture hashes and a full
standard `abc` digest check. Private SHA/stream APIs currently have compile-only
default-return stubs; the incremental/stream assertions intentionally remain red.
The existing SHA, atomic writer, and snapshot paths retain their behavior.

Reproduce the focused Release check with:

```sh
cmake -S . -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCHIPCAE_BUILD_TESTS=ON -DCHIPCAE_ENABLE_HDF5=ON -DCHIPCAE_ENABLE_JSONCONS=ON
ninja -C "$build" -j1 chipcae-jobmgr \
  chipcae_autotest_tests_protocol_jobmgr_local_jobmgr_local_snapshot_stream \
  chipcae_autotest_tests_protocol_jobmgr_local_jobmgr_local_layout
ctest --test-dir "$build" -j1 -V -R '^jobmgr_local_(snapshot_stream|layout)$'
```

Supply the dependency prefix for protobuf/jsoncons/Qt Core on the local machine.
Use the shared-slot shim first in PATH where required. Detailed commands and red
output are retained externally in `jobs-snapshot-stream-tdd.md` and its adjacent
logs. The external M1/M2/M3/T1/T2 benchmarks and main/branch byte comparison have
not been measured in this TEST-FIRST pass; no performance gate is claimed passed.

Final measured Release regression selection `ctest --test-dir "$build" -j1 -V
-R '^jobmgr_'`: 15 Passed, 1 expected Failed (`jobmgr_local_snapshot_stream`),
zero Skipped and zero timeouts. Layout and Qt desktop parity both passed.
The final RSS difference was 144476 KiB. A separate protobuf-disabled Release
build compiled the final test source and verified the intended return-77 skip.
Pinned clang-format 23.1.5 dry-run/Werror checks passed on all touched C++ files.
