# jobmgr

`chipcae::jobmgr` provides UUID, snapshot-reference, SHA-256 and atomic-file helpers
and always builds with `chipcae::core`. When the local manager dependencies are
available, `chipcae-jobmgr` exposes its submit, query, wait, cancel and poll calls
without Qt.

## CLI

Commands and options are case sensitive. `--root` must precede the command;
command options may appear in any order. Quote paths containing spaces.

```text
chipcae-jobmgr --help
chipcae-jobmgr -h
chipcae-jobmgr --version
chipcae-jobmgr -v
chipcae-jobmgr --root <dir> submit --case <case.json> --profile <profile.json> --request-id <uuid> [--solver <path>] [--mpi <path>] [--wait [--timeout-ms <n>]]
chipcae-jobmgr --root <dir> list
chipcae-jobmgr --root <dir> get --run-id <uuid>
chipcae-jobmgr --root <dir> wait --run-id <uuid> [--timeout-ms <n>]
chipcae-jobmgr --root <dir> cancel --run-id <uuid> [--grace-ms <n>]
chipcae-jobmgr --root <dir> poll
```

For example, run a synthetic case to completion in the submitting process:

```sh
chipcae-jobmgr --root "demo runs" submit \
  --case "synthetic inputs/case.json" --profile "synthetic inputs/profile.json" \
  --request-id 12345678-1234-4abc-8def-123456789abc \
  --solver chipcae-solve --wait --timeout-ms 60000
chipcae-jobmgr --root "demo runs" list
# Use the run_id printed by submit:
chipcae-jobmgr --root "demo runs" get --run-id "$RUN_ID"
chipcae-jobmgr --root "demo runs" wait --run-id "$RUN_ID" --timeout-ms 0
chipcae-jobmgr --root "demo runs" cancel --run-id "$RUN_ID" --grace-ms 1000
chipcae-jobmgr --root "demo runs" poll
```

`submit`, `get`, `wait` and `cancel` print a `jobs.Job` in protobuf text format;
`list` prints `ListJobsResponse` (an empty list can produce empty stdout).
`poll` prints exactly `consumed=<n>` followed by a newline. Solver stdout is
redirected to stderr, keeping stdout parseable. Retry the same exact request-ID
string with the same root to retrieve its stored Job without creating another run.
UUID validation is performed by the library. Defaults are solver `chipcae-solve`,
MPI launcher `mpirun`, and CUDA unavailable.

`submit --wait` calls `Submit` then `wait` in the same process and prints the
wait result. `--timeout-ms` is accepted for submit only with `--wait`. Timeout
and cancel grace default to zero; values must be signed, representable integer
milliseconds. A nonpositive timeout polls once. Grace is passed as a protobuf
Duration and clamped by the library to 0–60000 ms. A timeout can return a RUNNING
Job, so use a sufficient positive timeout to finish a solver.

The CLI does not host background jobs. Each invocation owns its solver children
and terminates unfinished children on exit. Without `--wait`, submit prints its
immediate Job and then shuts down the solver. A later invocation recovers that
nonterminal Job as FAILED with `FAILURE_CATEGORY_JOB_MANAGER` and message
`Launcher restarted without process ownership`. This also applies if
`submit --wait` times out before completion. A separate `wait --run-id` only
observes a persisted terminal Job; `cancel` and `poll` cannot control children
from a previous invocation. Successful terminal Jobs persist across subsequent
`get`, `wait` and `cancel` calls. These are the frozen library ownership semantics.

Exit codes:

- **0**: help/version, or a returned API response. A returned FAILED or CANCELLED
  Job is still exit 0; inspect its state, completion and failure fields. Case
  validation and launch errors can be returned as FAILED Jobs.
- **1**: library or runtime exception, with `what()` on stderr (including invalid
  request UUIDs and unknown run IDs), or unavailable local manager support.
- **2**: invalid grammar, with a short error and usage on stderr. This includes
  no arguments, missing/duplicate/unknown options, extra positional arguments,
  wrong case and malformed or overflowing milliseconds. Parsing precedes manager
  construction and dependency availability checks.

## Local manager library and availability

When Protobuf and jsoncons are available, `chipcae::jobmgr_local` provides the
Qt-free `chipcae::jobmgr::LocalJobManager`. CMake tries Protobuf CONFIG then module
mode, uses `protobuf::protoc` to generate only run/jobs messages into the jobmgr
build directory, and links no gRPC or Qt targets. The generated protocol library
links only protobuf. Without either dependency CMake reports:

```
jobmgr: LocalJobManager skipped (Protobuf or jsoncons not found)
```

Path tests and core still run; manager acceptance tests return 77. The launch,
cancel and desktop parity tests are always registered. Without the local target
all three skip with code 77; without Qt Core only desktop parity skips, printing
`SKIP: desktop Qt sources unavailable`. The mock solver is built only with the
local target. Desktop parity compiles the original desktop sources against the
same generated protocol library.

The CLI still builds without the local target. Both help and version aliases
succeed; valid operational commands exit 1 with exactly
`LocalJobManager not available in this build` on stderr. Grammar errors still
exit 2. All four `jobmgr_cli_*` CTest acceptance tests are registered with
`tierA`, a 60-second timeout and skip code 77 when the local target is absent;
their mock solver is built only when it is present.

Construct with `LocalConfig{run_root, solver, mpi, cuda_available}`. One manager
owns each absolute run root via `launcher.lock`. Each run has an atomic `job.pb`,
the original successful `submit.pb`, raw input/profile snapshots, snapshot hashes,
and an append-only `control.jsonl`. Exact request-ID retries return the stored Job
or accepted ControlReply, including after restart. Rejected controls are not cached.
Runs and List results are ordered by run ID. The manager assumes serialized calls
from its owner; callers must synchronize access across threads.

Call `poll()` to consume process exits and persist outcomes. It calls `reap()` once
and returns the number of consumed exit records, including unknown IDs and terminal
duplicates; those records do not change terminal Jobs. `Control` and `Cancel` poll
first. `Get`, `List` and `ListOutputs` are const observations and never poll. On
restart, nonterminal Jobs become FAILED/JOB_MANAGER because the new manager does
not own their processes. Destruction terminates and collects still-owned children
before releasing the root lock, subject to the shutdown budget described below.

The default POSIX launcher uses `posix_spawnp` without shell parsing. It preserves
argument boundaries, inherits the working directory and stdout/stderr, and copies
the system environment with profile overrides without changing the parent.
Missing or non-executable programs fail synchronously as FAILED/LAUNCH with
`failed to start <program>: <system error>`.

`wait(run_id, timeout)` polls once, then polls at intervals of at most 10 ms until
the Job is terminal or the steady-clock timeout expires. Nonpositive timeouts
poll exactly once; unknown IDs throw `Unknown run_id`. CANCELLED is terminal, so
wait returns promptly even when its child still awaits collection.

Cancel persists CANCELLED and sends SIGTERM. Positive grace uses an independent
condition-variable watchdog to send SIGKILL on expiry; zero grace sends SIGKILL
immediately. The manager clamps grace to 0–60000 ms. Only owned PIDs are collected,
and each exit is returned once in run-ID order. Call poll after cancellation to
collect the child; watchdogs signal but do not reap. Destruction wakes and joins
watchdogs, kills outstanding children, and collects within one shared three-second
budget before releasing `launcher.lock`. In the exceptional OS case where a
SIGKILLed child remains uninterruptible beyond that budget, destruction returns
without claiming that it was collected; the process may remain until the OS
allows its exit. A strict time bound cannot guarantee collection in that case.

See `tests/protocol/jobmgr_local/README.md` for the acceptance matrix. Desktop
remains on its existing LocalJobService for the December demo and does not call
this manager.

## 与 desktop 的已知差异

- Case validation checks the case schema (including the locally resolved
  `parameters.schema.json` reference), materials/stack reference existence, the
  stack schema, and duplicate string IDs throughout case objects outside `x-`
  subtrees. Desktop currently limits its duplicate-ID walk to loads/constraints.
- Qt-dependent material-library content validation (`libraryCheck`, including
  material properties and IDs), duplicate stack layer names, unknown target layers,
  and layer/region material-ID cross-checks are omitted here. Snapshot reference
  safety, recursive copying, supplied hashes and raw case hashes are checked.
- Malformed label selectors reject even for an empty or fully filtered List.
  Desktop validates selectors during iteration. A selector must contain exactly
  one `=`; paging emits a token only if another matching Job exists.
- State updates use explicit `poll()` instead of Qt process-signal dispatch.
  Mock event-file and JSONL event branches specific to desktop tests are not part
  of LocalConfig.
- Lock reclamation is conservative: only a demonstrably dead PID on the same host
  is reclaimed. A live/reused PID, remote host, malformed owner or inaccessible
  PID blocks acquisition; inode checks and nonblocking flock protect reclamation.

These content checks and desktop migration remain for the post-demo switching
card. This implementation consumes frozen I7-jobs-local/1.0 §1–§4 and does not
implement its unfrozen remote services or scheduler states.
