"""Catch timing data leaking to disk or nondeterministic serialization."""

import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def main():
    helper, stackup, fixture = sys.argv[1:]
    with open(fixture, encoding="utf-8") as stream:
        invariant = json.load(stream)["ondisk_invariant"]
    with tempfile.TemporaryDirectory(prefix="iowt-files-") as temporary:
        runs = [Path(temporary) / name for name in ("first", "second")]
        for run in runs:
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
            subprocess.run(
                [helper, "inspect", str(run / "out/step-0.vtkhdf")]
                + invariant["vtkhdf_forbidden_names"],
                check=True,
            )
        for relative in ("manifest.json", "out/step-0.vtkhdf"):
            digests = [hashlib.sha256((run / relative).read_bytes()).hexdigest() for run in runs]
            require(digests[0] == digests[1], f"whole-file SHA-256 differs: {relative}: {digests}")


if __name__ == "__main__":
    main()
