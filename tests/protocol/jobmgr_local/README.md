These acceptance tests cover both cards of I7-jobs-local/1.0. They use explicit
`require` checks in Release builds. `test_paths` always runs; the other manager
tests skip with code 77 only when the optional local target is absent.

The launch card is TEST-FIRST: the default launcher is still unavailable and
`LocalJobManager::wait` has only a compilation stub returning `Get`. The new
launch and cancellation executables intentionally fail their behavior assertions
until the launcher and waiting implementation arrive. They are ordinary failures,
not expected-failure or skip registrations. Existing state tests inject their
failing FakeSpawner explicitly rather than depending on the default launcher.

`test_launch_e2e` uses the production EventLogWriter in a standalone mock solver
for success reasons, missing/torn/invalid/failed/no exit logs, exit 9, signal 139,
launch errors, and fake MPI. It compares returned/Get/persisted jobs, captured
argv, PID, OMP overrides, inherited environment and working directory, including
spaced/UTF-8 paths and executable PATH lookup. Three simultaneous children across
two managers check that reaping neither steals another owner's exit nor returns
an exit twice. It never runs a real solver or real mpirun.

`test_cancel` checks TERM, grace-expiry KILL independent of polling, zero grace,
reaping, sticky CANCELLED state, manager destruction and recovery, and wait
boundaries, including a positive timeout with FakeSpawner so the wait stub is
tested independently of the missing real launcher. The Linux `/proc` zombie
check waits for mock readiness published after installing SIGTERM handling.
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
TDD evidence log records commands and observed failures.

Recovery makes every formerly live job terminal. New Control calls after
restart therefore reject; cached accepted replies retain their original uint64
sequence (including values beyond double precision). The malformed label
selector check also applies to an empty root, stricter than desktop validation
inside iteration.
