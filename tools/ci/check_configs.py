"""Validate the perf-gate config and the protocol error table."""

from __future__ import annotations

import sys
from pathlib import Path

import yaml

REQUIRED_REPETITIONS = {
    "microbenchmark": {"warmup": 1, "n": 7, "aggregate": "median"},
    "end_to_end_cpu": {"warmup": 0, "n": 3, "aggregate": "median"},
    "end_to_end_gpu": {"warmup": 1, "n": 3, "aggregate": "median"},
    "scaling": {"warmup": 0, "n": 2, "aggregate": "best"},
}


def _load(path: Path):
    with path.open(encoding="utf-8") as handle:
        return yaml.safe_load(handle)


def check_perf(path: Path) -> list[str]:
    errors = []
    data = _load(path)
    if not isinstance(data, dict):
        return [f"{path}: expected a mapping"]
    repetitions = data.get("repetitions") or {}
    for name, expected in REQUIRED_REPETITIONS.items():
        if repetitions.get(name) != expected:
            errors.append(f"{path}: repetitions.{name} is {repetitions.get(name)!r}, expected {expected!r}")
    noise = data.get("noise") or {}
    if noise.get("cv_max") != 0.05 or noise.get("reruns") != 1:
        errors.append(f"{path}: noise policy must stay cv_max=0.05 and reruns=1")
    baseline = data.get("baseline") or {}
    if baseline.get("auto_update") is not False:
        errors.append(f"{path}: baseline.auto_update must be false")
    if baseline.get("update_label") != "perf-baseline-update":
        errors.append(f"{path}: baseline update label drifted")
    if baseline.get("per_commit_k") != 20 or baseline.get("nightly_k") != 10:
        errors.append(f"{path}: baseline window must stay K=20 per-commit and K=10 nightly")
    thresholds = data.get("thresholds") or {}
    per_commit = thresholds.get("per_commit") or {}
    if per_commit.get("wall_time", {}).get("blocks") is not False:
        errors.append(f"{path}: per-commit wall time must not block")
    if per_commit.get("iterations", {}).get("warn") != 0.02 or per_commit.get("iterations", {}).get("block") != 0.10:
        errors.append(f"{path}: per-commit iteration thresholds drifted")
    if per_commit.get("peak_memory", {}).get("warn") != 0.05 or per_commit.get("peak_memory", {}).get("block") != 0.10:
        errors.append(f"{path}: per-commit memory thresholds drifted")
    nightly = thresholds.get("nightly") or {}
    if nightly.get("wall_time", {}).get("block_next_day") != 0.15:
        errors.append(f"{path}: nightly wall-time next-day hold must stay 15%")
    if not (data.get("tiers") or {}).get("a", {}).get("timing_informational"):
        errors.append(f"{path}: tier A timing must stay informational")
    return errors


def check_errors(path: Path) -> list[str]:
    data = _load(path)
    if not isinstance(data, dict) or not isinstance(data.get("errors"), list):
        return [f"{path}: expected a mapping with an errors list"]
    seen = set()
    errors = []
    for item in data["errors"]:
        if not isinstance(item, dict) or "code" not in item or "name" not in item:
            errors.append(f"{path}: each error needs code and name")
            continue
        if item["code"] in seen:
            errors.append(f"{path}: duplicate code {item['code']}")
        seen.add(item["code"])
    return errors


def main() -> int:
    root = Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()
    problems = []
    problems.extend(check_perf(root / "bench" / "perf_config.yaml"))
    problems.extend(check_errors(root / "protocol" / "errors.yaml"))
    if problems:
        print("config check failed:")
        for problem in problems:
            print(f"  {problem}")
        return 1
    print("config check passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
