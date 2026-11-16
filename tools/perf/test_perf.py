"""Unit tests for the perf harness and the baseline comparison."""

from __future__ import annotations

import json
import math
import tempfile
import unittest
from pathlib import Path

from tools.perf.append_baseline import append_sample
from tools.perf.compare_baseline import evaluate, regression_ratio
from tools.perf.harness import apply_noise_policy, build_summary, load_config, summarize_records
from tools.perf.stats import coefficient_of_variation, mad, median

ROOT = Path(__file__).resolve().parents[2]
CONFIG = load_config(ROOT / "bench" / "perf_config.yaml")


def _record(**overrides):
    payload = {
        "benchmark": "poisson_cg",
        "kind": "microbenchmark",
        "implemented": True,
        "dof": 1000,
        "phases_s": {"mesh": 0.01, "assembly": 0.02, "solve": 0.04, "io": 0.001},
        "wall_s": 0.071,
        "dof_per_s": 25000.0,
        "linear_iterations": 40,
        "newton_iterations": 1,
        "peak_rss_bytes": 40_000_000,
        "peak_gpu_bytes": 0,
        "linear_relative_residual": 1e-14,
        "newton_tolerance": 0.0,
    }
    payload.update(overrides)
    return payload


def _summary_from(records, kind="microbenchmark"):
    policy = {
        "warmup": CONFIG["repetitions"][kind]["warmup"],
        "n": len(records),
        "aggregate": CONFIG["repetitions"][kind]["aggregate"],
    }
    return build_summary(
        kind=kind,
        records=records,
        policy=policy,
        reruns_used=0,
        inconclusive=[],
        runner_label="ubuntu-latest",
        build_config="gcc-release",
        config=CONFIG,
    )


def _baseline_from(record, copies=4):
    sample = {
        "runner_label": "ubuntu-latest",
        "build_config": "gcc-release",
        "wall_s": record["wall_s"],
        "dof_per_s": record["dof_per_s"],
        "linear_iterations": record["linear_iterations"],
        "newton_iterations": record["newton_iterations"],
        "peak_rss_bytes": record["peak_rss_bytes"],
        "peak_gpu_bytes": record["peak_gpu_bytes"],
    }
    return {"runner_label": "ubuntu-latest", "build_config": "gcc-release", "samples": [dict(sample) for _ in range(copies)]}


class StatsTest(unittest.TestCase):
    def test_median_odd_and_even(self):
        self.assertEqual(median([3, 1, 2]), 2)
        self.assertEqual(median([1, 2, 3, 4]), 2.5)

    def test_mad(self):
        self.assertEqual(mad([1, 2, 3, 4, 5]), 1)

    def test_cv_zero_when_constant(self):
        self.assertEqual(coefficient_of_variation([2, 2, 2]), 0.0)


class ConfigDefaultsTest(unittest.TestCase):
    def test_architect_defaults(self):
        reps = CONFIG["repetitions"]
        self.assertEqual(reps["microbenchmark"], {"warmup": 1, "n": 7, "aggregate": "median"})
        self.assertEqual(reps["end_to_end_cpu"], {"warmup": 0, "n": 3, "aggregate": "median"})
        self.assertEqual(reps["end_to_end_gpu"], {"warmup": 1, "n": 3, "aggregate": "median"})
        self.assertEqual(reps["scaling"], {"warmup": 0, "n": 2, "aggregate": "best"})
        self.assertEqual(CONFIG["noise"]["cv_max"], 0.05)
        self.assertEqual(CONFIG["noise"]["reruns"], 1)
        self.assertEqual(CONFIG["baseline"]["per_commit_k"], 20)
        self.assertEqual(CONFIG["baseline"]["nightly_k"], 10)
        self.assertFalse(CONFIG["baseline"]["auto_update"])
        self.assertEqual(CONFIG["baseline"]["update_label"], "perf-baseline-update")
        self.assertEqual(CONFIG["baseline"]["improvement_hint"], 0.10)
        per_commit = CONFIG["thresholds"]["per_commit"]
        self.assertEqual(per_commit["iterations"]["warn"], 0.02)
        self.assertEqual(per_commit["iterations"]["block"], 0.10)
        self.assertTrue(per_commit["iterations"]["blocks"])
        self.assertEqual(per_commit["peak_memory"]["warn"], 0.05)
        self.assertEqual(per_commit["peak_memory"]["block"], 0.10)
        self.assertTrue(per_commit["peak_memory"]["blocks"])
        self.assertFalse(per_commit["wall_time"]["blocks"])
        self.assertEqual(per_commit["wall_time"]["mad_band"], 3)
        nightly = CONFIG["thresholds"]["nightly"]
        self.assertEqual(nightly["wall_time"]["warn"], 0.05)
        self.assertEqual(nightly["wall_time"]["block_next_day"], 0.15)
        self.assertEqual(nightly["dof_per_s"]["block_next_day"], 0.15)
        self.assertEqual(nightly["iterations"]["block"], 0.10)
        self.assertTrue(CONFIG["tiers"]["a"]["timing_informational"])


class HarnessTest(unittest.TestCase):
    def test_median_aggregate(self):
        records = [_record(wall_s=value, linear_iterations=40) for value in (0.05, 0.07, 0.09)]
        summary = summarize_records(records, {"n": 3, "aggregate": "median"})
        self.assertAlmostEqual(summary["metrics"]["wall_s"]["current"], 0.07)

    def test_scaling_takes_the_faster_run(self):
        slow = _record(wall_s=0.2, linear_iterations=50, peak_rss_bytes=80)
        fast = _record(wall_s=0.1, linear_iterations=40, peak_rss_bytes=70)
        summary = summarize_records([slow, fast], {"n": 2, "aggregate": "best"})
        self.assertEqual(summary["current"]["wall_s"], 0.1)
        self.assertEqual(summary["current"]["linear_iterations"], 40)
        self.assertEqual(summary["current"]["peak_rss_bytes"], 70)

    def test_rerun_once_when_wall_cv_is_high(self):
        calls = {"n": 0}

        def execute_once():
            calls["n"] += 1
            # First set of 7 (after 1 warmup) is scattered. The rerun set is flat.
            if calls["n"] <= 8:
                wall = 0.01 if calls["n"] % 2 == 0 else 0.05
            else:
                wall = 0.02
            return _record(wall_s=wall)

        records, used, inconclusive = apply_noise_policy(
            execute_once,
            {"warmup": 1, "n": 7, "aggregate": "median"},
            cv_max=0.05,
            reruns=1,
        )
        self.assertEqual(used, 1)
        self.assertEqual(len(records), 7)
        self.assertEqual(inconclusive, [])
        self.assertEqual(calls["n"], 1 + 7 + 1 + 7)

    def test_still_noisy_is_inconclusive_and_not_a_hard_failure(self):
        def execute_once():
            execute_once.count += 1
            wall = 0.01 if execute_once.count % 2 == 0 else 0.2
            return _record(wall_s=wall, linear_iterations=40, peak_rss_bytes=1000)

        execute_once.count = 0
        _records, used, inconclusive = apply_noise_policy(
            execute_once,
            {"warmup": 1, "n": 7, "aggregate": "median"},
            cv_max=0.05,
            reruns=1,
        )
        self.assertEqual(used, 1)
        self.assertIn("wall_s", inconclusive)


class CompareTest(unittest.TestCase):
    def test_iteration_warn_and_block(self):
        base = _record()
        summary = _summary_from([_record(linear_iterations=41)])
        report = evaluate(summary, _baseline_from(base), CONFIG, profile="per-commit", tier="a")
        row = next(item for item in report["metrics"] if item["metric"] == "linear_iterations")
        self.assertEqual(row["status"], "warn")
        self.assertFalse(row["blocks"])
        self.assertFalse(report["blocks"])

        summary = _summary_from([_record(linear_iterations=45)])
        report = evaluate(summary, _baseline_from(base), CONFIG, profile="per-commit", tier="a")
        row = next(item for item in report["metrics"] if item["metric"] == "linear_iterations")
        self.assertEqual(row["status"], "block")
        self.assertTrue(report["blocks"])

    def test_memory_warn_and_block(self):
        base = _record()
        summary = _summary_from([_record(peak_rss_bytes=42_400_000)])
        report = evaluate(summary, _baseline_from(base), CONFIG, profile="per-commit", tier="a")
        row = next(item for item in report["metrics"] if item["metric"] == "peak_rss_bytes")
        self.assertEqual(row["status"], "warn")
        self.assertFalse(report["blocks"])

        summary = _summary_from([_record(peak_rss_bytes=45_000_000)])
        report = evaluate(summary, _baseline_from(base), CONFIG, profile="per-commit", tier="a")
        self.assertTrue(report["blocks"])

    def test_hosted_wall_time_never_blocks(self):
        base = _record()
        summary = _summary_from([_record(wall_s=base["wall_s"] * 2, dof_per_s=base["dof_per_s"] / 2)])
        report = evaluate(summary, _baseline_from(base), CONFIG, profile="per-commit", tier="a")
        wall = next(item for item in report["metrics"] if item["metric"] == "wall_s")
        self.assertEqual(wall["status"], "informational")
        self.assertFalse(wall["blocks"])
        self.assertFalse(report["blocks"])
        self.assertFalse(report["block_next_day"])

    def test_tier_b_per_commit_wall_does_not_block_even_outside_mad(self):
        base = _record()
        summary = _summary_from([_record(wall_s=base["wall_s"] * 1.2)])
        report = evaluate(summary, _baseline_from(base), CONFIG, profile="per-commit", tier="b")
        wall = next(item for item in report["metrics"] if item["metric"] == "wall_s")
        self.assertFalse(wall["blocks"])
        self.assertFalse(report["blocks"])
        self.assertTrue(wall["legacy_would_block"])

    def test_nightly_wall_sets_next_day_hold_without_failing_the_sample(self):
        base = _record()
        summary = _summary_from([_record(wall_s=base["wall_s"] * 1.2, dof_per_s=base["dof_per_s"] / 1.2)])
        report = evaluate(summary, _baseline_from(base), CONFIG, profile="nightly", tier="b")
        wall = next(item for item in report["metrics"] if item["metric"] == "wall_s")
        self.assertEqual(wall["status"], "block_next_day")
        self.assertFalse(wall["blocks"])
        self.assertTrue(report["block_next_day"])
        self.assertFalse(report["blocks"])

    def test_hosted_nightly_timing_does_not_hold_the_next_day(self):
        base = _record()
        summary = _summary_from([_record(wall_s=base["wall_s"] * 2)])
        report = evaluate(summary, _baseline_from(base), CONFIG, profile="nightly", tier="a")
        wall = next(item for item in report["metrics"] if item["metric"] == "wall_s")
        self.assertEqual(wall["status"], "informational")
        self.assertFalse(report["block_next_day"])

    def test_inconclusive_timing_does_not_block(self):
        base = _record()
        summary = _summary_from([_record(wall_s=base["wall_s"] * 3)])
        summary["inconclusive_metrics"] = ["wall_s", "dof_per_s"]
        report = evaluate(summary, _baseline_from(base), CONFIG, profile="nightly", tier="b")
        wall = next(item for item in report["metrics"] if item["metric"] == "wall_s")
        self.assertEqual(wall["status"], "inconclusive")
        self.assertTrue(report["inconclusive"])
        self.assertFalse(report["block_next_day"])
        self.assertFalse(report["blocks"])

    def test_no_baseline_does_not_block(self):
        summary = _summary_from([_record()])
        report = evaluate(summary, {"samples": []}, CONFIG, profile="per-commit", tier="a")
        self.assertEqual(report["status"], "no_baseline")
        self.assertFalse(report["blocks"])

    def test_improvement_hint(self):
        base = _record()
        summary = _summary_from([_record(linear_iterations=30)])
        report = evaluate(summary, _baseline_from(base), CONFIG, profile="per-commit", tier="a")
        row = next(item for item in report["metrics"] if item["metric"] == "linear_iterations")
        self.assertTrue(row["improvement_hint"])
        self.assertTrue(report["improvement_hint"])
        self.assertFalse(report["blocks"])

    def test_last_k_window(self):
        samples = []
        for iterations in (10, 10, 10, 80):
            samples.append(
                {
                    "runner_label": "ubuntu-latest",
                    "build_config": "gcc-release",
                    "wall_s": 0.071,
                    "dof_per_s": 25000.0,
                    "linear_iterations": iterations,
                    "newton_iterations": 1,
                    "peak_rss_bytes": 40_000_000,
                    "peak_gpu_bytes": 0,
                }
            )
        config = json.loads(json.dumps(CONFIG))
        config["baseline"]["per_commit_k"] = 3
        summary = _summary_from([_record(linear_iterations=10)])
        report = evaluate(summary, {"samples": samples}, config, profile="per-commit", tier="a")
        row = next(item for item in report["metrics"] if item["metric"] == "linear_iterations")
        self.assertEqual(row["baseline"], 10)
        self.assertEqual(row["status"], "ok")

    def test_stub_does_not_gate(self):
        summary = _summary_from([_record(implemented=False)])
        summary["implemented"] = False
        report = evaluate(summary, _baseline_from(_record()), CONFIG, profile="per-commit", tier="b")
        self.assertEqual(report["status"], "not_implemented")
        self.assertFalse(report["blocks"])

    def test_regression_ratio_signs(self):
        self.assertAlmostEqual(regression_ratio(11, 10, True), 0.1)
        self.assertAlmostEqual(regression_ratio(9, 10, False), 0.1)
        self.assertEqual(regression_ratio(0, 0, True), 0.0)
        self.assertTrue(math.isinf(regression_ratio(1, 0, True)))


class AppendBaselineTest(unittest.TestCase):
    def test_append_refuses_inconclusive(self):
        summary = _summary_from([_record()])
        summary["inconclusive_metrics"] = ["wall_s"]
        with self.assertRaises(SystemExit):
            append_sample({"samples": []}, summary, commit="abc", recorded_at="2026-11-16T00:00:00Z")

    def test_append_writes_current_values(self):
        summary = _summary_from([_record()])
        updated = append_sample({"samples": []}, summary, commit="abc", recorded_at="2026-11-16T00:00:00Z")
        self.assertEqual(updated["samples"][0]["commit"], "abc")
        self.assertEqual(updated["samples"][0]["linear_iterations"], 40)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "baseline.json"
            path.write_text("{}\n", encoding="utf-8")


if __name__ == "__main__":
    unittest.main()
