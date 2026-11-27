"""Catch timing data leaking to disk or nondeterministic serialization."""

import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import time


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def main():
    helper, stackup, fixture = sys.argv[1:]
    with open(fixture, encoding="utf-8") as stream:
        expected = json.load(stream)
    invariant = expected["ondisk_invariant"]
    steps = expected["synthetic_setup"]["steps"]
    step_files = [Path("out") / f"step-{step}.vtkhdf" for step in steps]
    with tempfile.TemporaryDirectory(prefix="iowt-files-") as temporary:
        runs = [Path(temporary) / name for name in ("first", "second")]
        for run in runs:
            if run == runs[1]:
                # Cross HDF5's timestamp boundary after the first run's inspections.
                second = int(time.time())
                while int(time.time()) == second:
                    time.sleep(0.01)
                time.sleep(0.05)
            subprocess.run([helper, "generate", stackup, str(run)], check=True)
            with (run / "manifest.json").open(encoding="utf-8") as stream:
                manifest = json.load(stream)
            require(
                set(manifest) == set(invariant["manifest_keys_required"]),
                f"unexpected manifest key set: {set(manifest)}",
            )
            require(
                not set(manifest).intersection(invariant["manifest_keys_forbidden"]),
                "timing key leaked into manifest",
            )
            require([step["n"] for step in manifest["steps"]] == steps, "unexpected steps")
            require(
                set((run / "out").glob("step-*.vtkhdf"))
                == {run / relative for relative in step_files},
                "unexpected step file set",
            )
            for relative in step_files:
                subprocess.run(
                    [helper, "inspect", str(run / relative)]
                    + invariant["vtkhdf_forbidden_names"],
                    check=True,
                )
        for relative in [Path("manifest.json"), *step_files]:
            digests = [hashlib.sha256((run / relative).read_bytes()).hexdigest() for run in runs]
            require(digests[0] == digests[1], f"whole-file SHA-256 differs: {relative}: {digests}")


if __name__ == "__main__":
    main()
