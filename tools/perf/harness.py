"""Repeat a benchmark binary and report median, MAD, and a noise flag.

The binary prints one JSON object. Warm-up runs are discarded. If the wall-time
coefficient of variation stays above the configured limit after the allowed
rerun, timing metrics are marked inconclusive and do not block.
"""

from __future__ import annotations

import argparse
import json
import math
import subprocess
import sys
from collections.abc import Callable, Mapping, Sequence
from pathlib import Path

if __package__:
    from .stats import coefficient_of_variation, mad, median
else:
    from stats import coefficient_of_variation, mad, median

METRIC_FIELDS = (
    "wall_s",
    "dof_per_s",
    "linear_iterations",
    "newton_iterations",
    "peak_rss_bytes",
    "peak_gpu_bytes",
)
PHASES = ("mesh", "assembly", "solve", "io")
TIMING_METRICS = ("wall_s", "dof_per_s")


def load_config(path: Path) -> dict:
    try:
        import yaml
    except ImportError as exc:  # pragma: no cover - runner image has PyYAML
        raise SystemExit("PyYAML is required to read the perf config") from exc
    with path.open(encoding="utf-8") as handle:
        data = yaml.safe_load(handle)
    if not isinstance(data, dict):
        raise SystemExit(f"{path} did not contain a mapping")
    return data


def _policy(config: Mapping, kind: str) -> dict:
    try:
        policy = config["repetitions"][kind]
    except KeyError as exc:
        raise SystemExit(f"perf config has no repetitions.{kind}") from exc
    aggregate = policy.get("aggregate", "median")
    if aggregate not in {"median", "best"}:
        raise SystemExit(f"unsupported aggregate {aggregate!r} for {kind}")
    return {
        "warmup": int(policy.get("warmup", 0)),
        "n": int(policy["n"]),
        "aggregate": aggregate,
    }


def _finite(value: object, field: str) -> float:
    number = float(value)  # type: ignore[arg-type]
    if not math.isfinite(number):
        raise SystemExit(f"benchmark field {field} is not finite: {value!r}")
    return number


def summarize_records(records: Sequence[Mapping], policy: Mapping) -> dict:
    if len(records) != policy["n"]:
        raise SystemExit(f"expected {policy['n']} records, got {len(records)}")
    if policy["aggregate"] == "best":
        chosen = min(records, key=lambda record: _finite(record["wall_s"], "wall_s"))
        current = {field: _finite(chosen[field], field) for field in METRIC_FIELDS}
    else:
        current = {
            field: median([_finite(record[field], field) for record in records])
            for field in METRIC_FIELDS
        }
    metrics = {}
    for field in METRIC_FIELDS:
        samples = [_finite(record[field], field) for record in records]
        metrics[field] = {
            "current": current[field],
            "median": median(samples),
            "mad": mad(samples),
            "cv": coefficient_of_variation(samples),
            "samples": samples,
        }
    phases = {}
    for phase in PHASES:
        samples = [_finite(record["phases_s"][phase], phase) for record in records]
        phases[phase] = {"median": median(samples), "mad": mad(samples), "samples": samples}
    return {"metrics": metrics, "phases_s": phases, "current": current}


def run_set(
    execute_once: Callable[[], Mapping],
    policy: Mapping,
) -> list[Mapping]:
    for _ in range(policy["warmup"]):
        execute_once()
    return [execute_once() for _ in range(policy["n"])]


def apply_noise_policy(
    execute_once: Callable[[], Mapping],
    policy: Mapping,
    *,
    cv_max: float,
    reruns: int,
) -> tuple[list[Mapping], int, list[str]]:
    """Return (records, reruns_used, inconclusive_metrics)."""
    records = run_set(execute_once, policy)
    used = 0
    def wall_cv(sample: Sequence[Mapping]) -> float:
        return coefficient_of_variation([_finite(record["wall_s"], "wall_s") for record in sample])

    if wall_cv(records) > cv_max and reruns > 0:
        records = run_set(execute_once, policy)
        used = 1
    summary = summarize_records(records, policy)
    inconclusive = [
        field
        for field in TIMING_METRICS
        if summary["metrics"][field]["cv"] > cv_max
    ]
    # Memory and iteration counts are gated on their own scatter, not on wall time.
    for field in ("linear_iterations", "newton_iterations", "peak_rss_bytes", "peak_gpu_bytes"):
        if summary["metrics"][field]["cv"] > cv_max:
            inconclusive.append(field)
    return records, used, inconclusive


def execute_binary(binary: Path, kind: str, grid: int) -> dict:
    completed = subprocess.run(
        [str(binary), "--kind", kind, "--grid", str(grid)],
        check=False,
        capture_output=True,
        text=True,
    )
    if completed.returncode != 0:
        sys.stderr.write(completed.stderr)
        raise SystemExit(f"{binary} --kind {kind} exited {completed.returncode}")
    try:
        payload = json.loads(completed.stdout)
    except json.JSONDecodeError as exc:
        raise SystemExit(f"benchmark did not print JSON: {completed.stdout!r}") from exc
    if payload.get("implemented"):
        residual = float(payload["linear_relative_residual"])
        if residual > 1e-12:
            raise SystemExit(f"linear relative residual {residual} exceeds 1e-12")
    return payload


def build_summary(
    *,
    kind: str,
    records: Sequence[Mapping],
    policy: Mapping,
    reruns_used: int,
    inconclusive: Sequence[str],
    runner_label: str,
    build_config: str,
    config: Mapping,
) -> dict:
    reduced = summarize_records(records, policy)
    implemented = all(bool(record.get("implemented", False)) for record in records)
    first = records[0]
    return {
        "schema_version": 1,
        "benchmark": first.get("benchmark", "poisson_cg"),
        "kind": kind,
        "implemented": implemented,
        "runner_label": runner_label,
        "build_config": build_config,
        "dof": first.get("dof", 0),
        "warmup_discarded": policy["warmup"],
        "repetitions": policy["n"],
        "aggregate": policy["aggregate"],
        "reruns": reruns_used,
        "cv_max": float(config["noise"]["cv_max"]),
        "inconclusive_metrics": list(inconclusive),
        "metrics": reduced["metrics"],
        "phases_s": reduced["phases_s"],
    }


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Run a chipcae benchmark kind N times.")
    parser.add_argument("--config", type=Path, default=Path("bench/perf_config.yaml"))
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--kind", required=True)
    parser.add_argument("--grid", type=int, default=48)
    parser.add_argument("--runner-label", default="ubuntu-latest")
    parser.add_argument("--build-config", default="gcc-release")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)

    config = load_config(args.config)
    allowed = set(config.get("tier_a_kinds", [])) | set(config.get("tier_b_kinds", []))
    if args.kind not in allowed:
        raise SystemExit(f"kind {args.kind!r} is not listed in the perf config")
    policy = _policy(config, args.kind)
    if policy["n"] < 1:
        raise SystemExit("repetition count must be positive")

    def once() -> dict:
        return execute_binary(args.binary, args.kind, args.grid)

    records, reruns_used, inconclusive = apply_noise_policy(
        once,
        policy,
        cv_max=float(config["noise"]["cv_max"]),
        reruns=int(config["noise"]["reruns"]),
    )
    # Stubs have no solver timing. Do not flag them inconclusive.
    if records and not records[0].get("implemented", False):
        inconclusive = []
    summary = build_summary(
        kind=args.kind,
        records=records,
        policy=policy,
        reruns_used=reruns_used,
        inconclusive=inconclusive,
        runner_label=args.runner_label,
        build_config=args.build_config,
        config=config,
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(
        f"{args.kind}: aggregate={policy['aggregate']} n={policy['n']} "
        f"reruns={reruns_used} inconclusive={list(inconclusive) or 'none'}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
