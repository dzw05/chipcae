"""Compare a perf summary with the stored baseline.

Exit status is 0 unless a per-commit blocking metric (iteration count or peak
memory) or the matching nightly metric regresses past its block threshold.
Wall time on Tier A is informational. Nightly wall time / DOF/s above the
configured ceiling sets block_next_day and does not fail this process.
A metric marked inconclusive by the harness does not block.
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from collections.abc import Mapping, Sequence
from pathlib import Path

if __package__:
    from .harness import load_config
    from .stats import mad, median
else:
    from harness import load_config
    from stats import mad, median


def _profile_key(profile: str) -> str:
    if profile == "per-commit":
        return "per_commit"
    if profile == "nightly":
        return "nightly"
    raise SystemExit(f"unknown profile {profile!r}")


def _window_k(config: Mapping, profile: str) -> int:
    baseline = config["baseline"]
    if profile == "per-commit":
        return int(baseline["per_commit_k"])
    if profile == "nightly":
        return int(baseline["nightly_k"])
    raise SystemExit(f"unknown profile {profile!r}")


def matching_samples(baseline: Mapping, runner_label: str, build_config: str, k: int) -> list[dict]:
    samples = [
        sample
        for sample in baseline.get("samples", [])
        if sample.get("runner_label", baseline.get("runner_label")) == runner_label
        and sample.get("build_config", baseline.get("build_config")) == build_config
    ]
    return samples[-k:]


def regression_ratio(current: float, reference: float, higher_is_worse: bool) -> float:
    if reference == 0.0 and current == 0.0:
        return 0.0
    if reference == 0.0:
        worse = current > 0.0 if higher_is_worse else current < 0.0
        return math.inf if worse else 0.0
    if higher_is_worse:
        return (current - reference) / abs(reference)
    return (reference - current) / abs(reference)


def _group_rules(config: Mapping, profile: str, group: str) -> dict:
    return dict(config["thresholds"][_profile_key(profile)][group])


def evaluate(
    summary: Mapping,
    baseline: Mapping,
    config: Mapping,
    *,
    profile: str,
    tier: str,
) -> dict:
    if config["baseline"].get("auto_update"):
        raise SystemExit("baseline.auto_update must stay false")
    tier_name = tier.lower()
    if tier_name not in {"a", "b"}:
        raise SystemExit("tier must be a or b")
    timing_informational = bool(config["tiers"][tier_name]["timing_informational"]) or tier_name == "a"

    if not summary.get("implemented", False):
        return {
            "schema_version": 1,
            "profile": profile,
            "tier": tier_name,
            "status": "not_implemented",
            "blocks": False,
            "inconclusive": False,
            "block_next_day": False,
            "improvement_hint": False,
            "warn": False,
            "metrics": [],
            "note": "This kind is a stub. It does not gate.",
        }

    k = _window_k(config, profile)
    samples = matching_samples(
        baseline,
        str(summary["runner_label"]),
        str(summary["build_config"]),
        k,
    )
    inconclusive_metrics = set(summary.get("inconclusive_metrics") or [])
    metric_rows = []
    if not samples:
        for name in config["metrics"]:
            metric_rows.append(
                {
                    "metric": name,
                    "group": config["metrics"][name]["group"],
                    "status": "no_baseline",
                    "blocks": False,
                    "current": summary["metrics"][name]["current"],
                }
            )
        return _overall(
            profile=profile,
            tier=tier_name,
            metrics=metric_rows,
            status="no_baseline",
            note=(
                "No baseline samples for this runner label and build config. "
                "Add them with a PR labeled perf-baseline-update. Nothing blocks."
            ),
        )

    for name, spec in config["metrics"].items():
        group = spec["group"]
        higher_is_worse = bool(spec["higher_is_worse"])
        rules = _group_rules(config, profile, group)
        current = float(summary["metrics"][name]["current"])
        reference_values = [float(sample[name]) for sample in samples]
        reference = median(reference_values)
        reference_mad = mad(reference_values)
        ratio = regression_ratio(current, reference, higher_is_worse)
        mad_band = float(rules.get("mad_band", 0) or 0)
        outside_mad = False
        if mad_band > 0 and higher_is_worse:
            outside_mad = current > reference + mad_band * reference_mad
        legacy_would_block = bool(ratio > float(rules.get("block", math.inf)) and outside_mad)
        row = {
            "metric": name,
            "group": group,
            "current": current,
            "baseline": reference,
            "baseline_mad": reference_mad,
            "baseline_samples": len(samples),
            "regression": ratio if math.isfinite(ratio) else "inf",
            "outside_mad_band": outside_mad,
            "legacy_would_block": legacy_would_block,
            "improvement_hint": math.isfinite(ratio) and ratio < -float(config["baseline"]["improvement_hint"]),
            "blocks": False,
            "status": "ok",
        }
        if name in inconclusive_metrics:
            row["status"] = "inconclusive"
            metric_rows.append(row)
            continue

        informational = timing_informational and group in {"wall_time", "dof_per_s"}
        warn_at = float(rules["warn"])
        if informational:
            row["status"] = "informational"
            row["warn_exceeded"] = bool(math.isfinite(ratio) and ratio > warn_at)
            metric_rows.append(row)
            continue

        block_at = rules.get("block")
        next_day_at = rules.get("block_next_day")
        if next_day_at is not None and ratio > float(next_day_at):
            # Hosted Tier A never raises the next-day hold from a timer.
            if tier_name == "a":
                row["status"] = "informational"
            else:
                row["status"] = "block_next_day"
        elif block_at is not None and ratio > float(block_at) and bool(rules.get("blocks", False)):
            row["status"] = "block"
            row["blocks"] = True
        elif ratio > warn_at:
            row["status"] = "warn"
        elif row["improvement_hint"]:
            row["status"] = "improvement"
        metric_rows.append(row)

    return _overall(profile=profile, tier=tier_name, metrics=metric_rows, status="compared")


def _overall(*, profile: str, tier: str, metrics: list[dict], status: str, note: str = "") -> dict:
    blocks = any(row.get("blocks") for row in metrics)
    inconclusive = any(row.get("status") == "inconclusive" for row in metrics)
    block_next_day = any(row.get("status") == "block_next_day" for row in metrics)
    improvement_hint = any(row.get("improvement_hint") for row in metrics)
    warn = any(row.get("status") == "warn" or row.get("warn_exceeded") for row in metrics)
    report = {
        "schema_version": 1,
        "profile": profile,
        "tier": tier,
        "status": "block" if blocks else status,
        "blocks": blocks,
        "inconclusive": inconclusive,
        "block_next_day": block_next_day,
        "improvement_hint": improvement_hint,
        "warn": warn,
        "metrics": metrics,
    }
    if note:
        report["note"] = note
    return report


def _write_github_output(path: Path, report: Mapping) -> None:
    lines = [
        f"blocks={'true' if report['blocks'] else 'false'}",
        f"inconclusive={'true' if report['inconclusive'] else 'false'}",
        f"block_next_day={'true' if report['block_next_day'] else 'false'}",
        f"improvement_hint={'true' if report['improvement_hint'] else 'false'}",
        f"warn={'true' if report['warn'] else 'false'}",
        f"status={report['status']}",
    ]
    with path.open("a", encoding="utf-8") as handle:
        handle.write("\n".join(lines) + "\n")


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Compare a perf summary with a baseline.")
    parser.add_argument("--config", type=Path, default=Path("bench/perf_config.yaml"))
    parser.add_argument("--summary", type=Path, required=True)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--profile", choices=("per-commit", "nightly"), required=True)
    parser.add_argument("--tier", choices=("a", "b"), required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--github-output", type=Path)
    args = parser.parse_args(argv)

    config = load_config(args.config)
    summary = json.loads(args.summary.read_text(encoding="utf-8"))
    baseline = json.loads(args.baseline.read_text(encoding="utf-8"))
    report = evaluate(summary, baseline, config, profile=args.profile, tier=args.tier)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    if args.github_output:
        _write_github_output(args.github_output, report)
    print(
        f"perf {args.profile} tier {args.tier}: status={report['status']} "
        f"blocks={report['blocks']} inconclusive={report['inconclusive']} "
        f"block_next_day={report['block_next_day']} improvement_hint={report['improvement_hint']}"
    )
    return 1 if report["blocks"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
