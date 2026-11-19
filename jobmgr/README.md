# jobmgr

`chipcae::jobmgr` provides UUID, snapshot-reference, SHA-256 and atomic-file helpers
and always builds with `chipcae::core`. The existing `chipcae-jobmgr` executable
retains its help/version interface; it does not submit jobs yet.

When Protobuf and jsoncons are available, `chipcae::jobmgr_local` provides the
Qt-free `chipcae::jobmgr::LocalJobManager`. CMake tries Protobuf CONFIG then module
mode, uses `protobuf::protoc` to generate only run/jobs messages into the jobmgr
build directory, and links no gRPC or Qt targets. The generated protocol library
links only protobuf. Without either dependency CMake reports:

```
jobmgr: LocalJobManager skipped (Protobuf or jsoncons not found)
```

Path tests and core still run; the six manager acceptance tests return 77.

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
not own their processes. Destruction requests termination of still-owned children
before releasing the spawner and root lock.

The default launcher is transitional: `start` returns no PID and the error
`process launcher not available in this build`. Inject `detail::Spawner` to supply
process operations; this card's acceptance tests inject FakeSpawner. Real POSIX
launch/wait, SIGTERM→SIGKILL, desktop command comparisons and bidirectional
QLockFile tests belong to the `jobs-local-launch` card. Desktop remains on its
existing LocalJobService for the December demo and does not call this manager.

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
  The default launcher is unavailable until the second card. Mock event-file and
  JSONL event branches specific to desktop tests are not part of LocalConfig.
- Lock reclamation is conservative: only a demonstrably dead PID on the same host
  is reclaimed. A live/reused PID, remote host, malformed owner or inaccessible
  PID blocks acquisition; inode checks and nonblocking flock protect reclamation.

These content checks and desktop migration remain for the post-demo switching
card. This implementation consumes frozen I7-jobs-local/1.0 §1–§4 and does not
implement its unfrozen remote services or scheduler states.
