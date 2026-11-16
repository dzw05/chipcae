"""Refuse edits to an existing perf baseline unless the PR has the update label.

A baseline that does not yet exist on the base branch may be added. CI never
rewrites the file.
"""

from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path


def requiring_label(changed: list[str], existed_on_base: set[str]) -> list[str]:
    return [path for path in changed if path in existed_on_base]


def git(args: list[str], cwd: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(args, cwd=cwd, check=False, capture_output=True, text=True)


def main() -> int:
    root = Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()
    base = os.environ.get("BASE_SHA") or os.environ.get("BASE_REF", "")
    if not base:
        print("BASE_SHA or BASE_REF is not set; skipping baseline label check.")
        return 0
    has_label = os.environ.get("HAS_BASELINE_LABEL", "false").lower() == "true"
    diff = git(["git", "diff", "--name-only", f"{base}...HEAD", "--", "bench/baselines"], root)
    if diff.returncode != 0:
        # Fall back to a two-dot diff if the merge base is unavailable.
        diff = git(["git", "diff", "--name-only", base, "HEAD", "--", "bench/baselines"], root)
    if diff.returncode != 0:
        print(diff.stderr)
        print("could not diff bench/baselines against the base revision")
        return 1
    changed = [line.strip() for line in diff.stdout.splitlines() if line.strip()]
    existed = set()
    for path in changed:
        probe = git(["git", "cat-file", "-e", f"{base}:{path}"], root)
        if probe.returncode == 0:
            existed.add(path)
    gated = requiring_label(changed, existed)
    if not gated:
        print("baseline label check passed.")
        return 0
    if has_label:
        print("baseline files changed and perf-baseline-update is set:")
        for path in gated:
            print(f"  {path}")
        return 0
    print("existing perf baseline files changed without the perf-baseline-update label:")
    for path in gated:
        print(f"  {path}")
    print("Baselines are not auto-updated. Open the change on a PR with that label.")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
