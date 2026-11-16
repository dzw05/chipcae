"""Append one successful main-branch sample to a baseline file.

CI must not call this. Baselines change only in a pull request labeled
perf-baseline-update. The script refuses to run when CI is set.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path


def append_sample(baseline: dict, summary: dict, *, commit: str, recorded_at: str) -> dict:
    if not summary.get("implemented", False):
        raise SystemExit("refusing to record a stub result")
    if summary.get("inconclusive_metrics"):
        raise SystemExit("refusing to record an inconclusive result")
    sample = {
        "commit": commit,
        "recorded_at": recorded_at,
        "runner_label": summary["runner_label"],
        "build_config": summary["build_config"],
    }
    for name, stats in summary["metrics"].items():
        sample[name] = stats["current"]
    sample["phases_s"] = {phase: values["median"] for phase, values in summary["phases_s"].items()}
    updated = dict(baseline)
    updated.setdefault("samples", [])
    updated["samples"] = list(updated["samples"]) + [sample]
    return updated


def main(argv: list[str] | None = None) -> int:
    if os.environ.get("CI") and "--allow-in-ci" not in (argv or sys.argv):
        raise SystemExit("baseline files are not updated from CI")
    parser = argparse.ArgumentParser(description="Append a perf sample to a baseline file.")
    parser.add_argument("--summary", type=Path, required=True)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--recorded-at", required=True)
    parser.add_argument("--allow-in-ci", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args(argv)
    baseline = json.loads(args.baseline.read_text(encoding="utf-8"))
    summary = json.loads(args.summary.read_text(encoding="utf-8"))
    updated = append_sample(baseline, summary, commit=args.commit, recorded_at=args.recorded_at)
    args.baseline.write_text(json.dumps(updated, indent=2) + "\n", encoding="utf-8")
    print(f"appended sample {args.commit} ({len(updated['samples'])} total)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
