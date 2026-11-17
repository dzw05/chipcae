# Desktop runtime configuration

`chipcae.desktop.config/0.1` is a desktop private JSON contract, outside the
shared schema and protocol directories. D14c loads and stores configuration;
UI wiring to controllers and monitoring belongs to D14b-2.

Loading selects exactly one source: `--config <path>` first, otherwise
`CHIPCAE_DESKTOP_CONFIG`, otherwise built-in defaults. Duplicate `--config`,
a missing argument, an empty selected path, unreadable files, malformed JSON,
an invalid schema or missing required fields are errors. No fallback occurs.
Other command-line arguments are left for Qt. Errors reach stderr and cause
nonzero exit before the application creates a window.

Built-in defaults use real entrypoints resolved from PATH: `chipcae-import`,
`chipcae-preview`, `chipcae-solve`, and MPI `mpirun`. Each source is `real`,
import/preview prefix arrays are empty, and `events_file` is `events.bin`.
The default `run_root` is the absolute `runs/` path under the working directory
at load time. Loading does not create it or launch any process.

A file must contain these required fields (defaults only apply when no file
is selected):

| Field | Type and meaning |
| --- | --- |
| `schema` | String, exactly `chipcae.desktop.config/0.1` |
| `run_root` | Nonempty string, run directory root |
| `entrypoints` | Object with `import`, `preview`, and `solve` objects |
| Each entry's `source` | String, `real` or `mock`, independently selected |
| Each entry's `program` | Nonempty string, executable path or PATH name |
| Import and preview `prefix` | Required array of strings, prepended controller arguments |
| Solve `mpi` | Nonempty string, MPI executable path or PATH name |
| Solve `events_file` | Nonempty string, event filename for later monitor/KernelExit wiring |

Optional import `material_map` is a nonempty string. Optional `precomputed`
is an object requiring nonempty `run_dir` and a 64-hex-digit `manifest_sha256`.
These are retained as metadata; this loader does not read those files or verify
hashes. Unknown fields are ignored for forward compatibility. Paths are preserved
verbatim; relative paths use the process working directory, not the config file's
directory. No shell expansion, executable existence check or directory creation
is performed.

Example real configuration:

```json
{
  "schema": "chipcae.desktop.config/0.1",
  "run_root": "/path/to/runs",
  "entrypoints": {
    "import": {"source": "real", "program": "chipcae-import", "prefix": [],
               "material_map": "material-map.json"},
    "preview": {"source": "real", "program": "chipcae-preview", "prefix": []},
    "solve": {"source": "real", "program": "chipcae-solve", "mpi": "mpirun",
              "events_file": "events.bin"}
  },
  "precomputed": {
    "run_dir": "/path/to/precomputed/run",
    "manifest_sha256": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
  }
}
```

For mock entrypoints, use e.g. import `program: "python3"`,
`prefix: ["desktop/tools/mock_import.py"]`, `source: "mock"`; preview similarly
uses `desktop/tools/mock_preview.py`. Solve uses a single executable adapter
such as `desktop/demo/bin/mock-solve` with `source: "mock"` and
`events_file: "events.caelog"`. Mixing mock import with real preview/solve is
allowed. The loader neither creates adapters nor rewrites commands.

Source checks are conservative filename checks: `source=real` rejects any
program or prefix element whose basename matches `mock_*.py` or exactly
`mock-solve`. `source=mock` rejects the known real executable basenames
`chipcae-import`, `chipcae-preview`, and `chipcae-solve`. Unknown wrappers and
custom executable names may use either source; these checks cannot establish
what an arbitrary executable actually does. File configuration is validated
before overrides and the effective configuration is validated again.

Environment overrides apply after selection:

| Program override | Required companion |
| --- | --- |
| `CHIPCAE_IMPORT` | `CHIPCAE_IMPORT_SOURCE` |
| `CHIPCAE_PREVIEW` | `CHIPCAE_PREVIEW_SOURCE` |
| `CHIPCAE_SOLVE` | `CHIPCAE_SOLVE_SOURCE` |
| `CHIPCAE_MPIRUN` (solve MPI) | `CHIPCAE_SOLVE_SOURCE` |

Presence, including an empty value, counts as an override. Empty programs and
invalid sources fail. Source variables alone may override source, subject to
the same mismatch checks. Prefix arguments remain unchanged when a program is
overridden. A missing companion is an error, never an implicit real/mock switch.

`DesktopConfig::load(args, environment)` is the shared CLI loading function.
`setCurrent` stores a successfully loaded process configuration; `current`
returns a const reference (built-in defaults if not yet set). Configuration
installation happens at startup before consumers use it.
`importEntry()` and `previewEntry()` expose program/prefix for existing
`ImportController::start(program, prefix)` and
`PreviewController::start(program, prefix)` APIs. `launcherConfig()` maps
solve program to `solver`, MPI to `mpi`, source to `mock`, and run_root to
`runRoot`, retaining other LauncherConfig defaults. `eventsFile()` exposes
solve.events_file separately: D14c does not add it to LauncherConfig or read
KernelExit. `solveEntry()`, `mpi()`, `runRoot()`, and `precomputed()` provide
read-only access to the remaining values.
