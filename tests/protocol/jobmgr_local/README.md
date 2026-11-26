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
The test-first layout repair derives fixture hashes from source bytes after a
full standard `abc` digest check. The prior acceptance commit records the failing
memory assertion and private-interface stubs; the streaming implementation now
passes those assertions without changing them.

Snapshot copying and SHA-256 share a fixed 1 MiB read buffer. The incremental
hash retains only its state and a partial 64-byte block. Both atomic writers use
the same temporary-file lifetime, complete-write loop and fsync/close/rename
sequence. Duplicate destinations still reread and verify their source without
rewriting the target. BOM/comment/object/array candidates retain the existing
whole-file JSON parser and reference traversal after the stream buffer is freed.
The case SHA comes from the already recorded snapshot hash.

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
Use the shared-slot shim first in PATH where required. Detailed test-first
commands and red output are retained externally in `jobs-snapshot-stream-tdd.md`
and adjacent logs.

Implementation validation (实测, Release, protobuf/jsoncons/HDF5/Qt Core enabled):
`ctest --test-dir "$build" -j1 -V -R '^jobmgr_'` reports **16 Passed, zero
Skipped, zero timeouts**. This includes CLI tests, layout and desktop parity.
The focused snapshot/layout run also passed. Its minimum-RSS delta was -52 KiB;
the 16-test regression run measured +120 KiB, both below 4096 KiB. A separate
protobuf-disabled, JSONCONS-off Release build compiles the snapshot test and
returns the intended 77 (`SKIP: LocalJobManager not built`). No timeout retries
were needed. Pinned clang-format 23.1.5 and `git diff --check` passed.

The external supplied benchmark is run unchanged, with 10 repetitions per size:

```sh
python3 "$evidence/jobs-snapshot-stream-bench.py" "$exe" "$repo" \
  "$evidence/jobs-snapshot-stream-branch-$revision.jsonl" 10 1 16 64 256
python3 "$evidence/jobs-snapshot-stream-bench.py" --json "$exe" "$repo" \
  "$evidence/jobs-snapshot-stream-branch-$revision-json64.jsonl" 10 64
cmake -S "$evidence/jobs-snapshot-stream-sha-build" -B "$sha_build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release
ninja -C "$sha_build" -j1
"$sha_build/sha-bench" 64 > "$evidence/jobs-snapshot-stream-branch-$revision-sha.jsonl"
```

The external SHA target links the supplied benchmark against this branch's
Release `libchipcae_jobmgr.a`. Its 64 MiB x 10 median is **213.51 MiB/s**, passing
T2's 191.96 MiB/s lower bound. No T2 rerun was needed.

Initial measurements (实测, numpy percentile with linear interpolation):

| Attachment | Wall median (s) | Peak RSS median (KiB) |
|---|---:|---:|
| Binary 1 MiB | 0.06590 | 15946 |
| Binary 16 MiB | 0.21550 | 15942 |
| Binary 64 MiB | 0.54145 | 15948 |
| Binary 256 MiB | 1.63115 | 15960 |
| JSON 64 MiB | 1.21475 | 146334 |

All 50 submissions verified output sizes, SHA-256 and manifest hashes. M1 passes:
the 1 MiB RSS Tukey upper bound is 16141.5 KiB, giving an 18189.5 KiB limit for every
larger binary size. M2 passes (15946 <= 16576 KiB). M3 passes
(146334 <= 210384 KiB); JSON wall time also passes its fixed 1.2867 s bound.

The initial 1/16 MiB T1 timings exceed their original bounds. Recorded host
load was 23.32, over twice the baseline's 10.6. The brief's permitted alternating
main/branch timing comparison is recorded separately; memory gates above retain
the original measurements and fixed limits.

The alternating run invokes the unchanged supplied benchmark once for main and
then once for branch in each of 10 repetitions, at all four binary sizes. All
80 submissions pass the integrity checks. T1 passes against the rerun main's
Q3 + 1.5 IQR bounds (实测):

| MiB | Main wall median (s) | Main Tukey upper bound (s) | Branch wall median (s) |
|---:|---:|---:|---:|
| 1 | 0.06800 | 0.0887875 | 0.06815 |
| 16 | 0.30430 | 0.5209000 | 0.21230 |
| 64 | 1.02205 | 1.5382375 | 0.48355 |
| 256 | 3.50515 | 4.9486250 | 2.11305 |

Reproduce that timing-only rerun with
`python3 "$evidence/jobs-snapshot-stream-alternate.py"`. The external driver,
per-invocation output, aggregate main/branch JSONL and timestamped load samples
are retained as `jobs-snapshot-stream-alternating-*`. Initial benchmark JSONL
and stats are retained as `jobs-snapshot-stream-branch-<commit>*`; the gate
summary records the original fixed memory limits and the timing rerun separately.

The separate retained-directory comparison uses one shared seeded 16 MiB source,
the same request ID, profile, solver and MPI settings for main and branch:
`python3 "$evidence/jobs-snapshot-stream-byte-equivalence/compare.py"`.
It checks raw equality of all four `inputs/**` files, `run.json` and `submit.pb`,
and compares raw manifest bytes after replacing only run-directory prefixes.
Protobuf Job equality removes only run ID, timestamps, result directory and
native job ID. All comparisons pass, including the identical full case SHA
`6933690ca67ebed3b7a7989056d8c26f167d1ff2d3d6058306e41094b8fb47ac`.
Rerunning this harness requires fresh main/branch output directories.
The harness, source and both run directories remain in the external
`jobs-snapshot-stream-byte-equivalence` evidence directory.

Scope follows the brief. The private header, test registration, acceptance tests
and layout repair were already supplied by the test-first commit and are reused
unchanged. `run_dir.hpp` and public interfaces need no changes. The brief's layout
citation is §2, while the frozen ruling places the layout in §1; behavior follows
the frozen ruling. The only validation adjustment is the explicitly permitted
load-triggered T1 rerun; no memory bound or timeout was changed.
