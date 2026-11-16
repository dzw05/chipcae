"""Tests for the CI config and baseline-label guards."""

from __future__ import annotations

import unittest
from pathlib import Path

from tools.ci.check_baseline_label import requiring_label
from tools.ci.check_configs import check_errors, check_perf

ROOT = Path(__file__).resolve().parents[2]


class CheckConfigTest(unittest.TestCase):
    def test_shipped_perf_config(self):
        self.assertEqual(check_perf(ROOT / "bench" / "perf_config.yaml"), [])

    def test_shipped_error_table(self):
        self.assertEqual(check_errors(ROOT / "protocol" / "errors.yaml"), [])


class BaselineLabelTest(unittest.TestCase):
    def test_new_file_does_not_need_the_label(self):
        self.assertEqual(requiring_label(["bench/baselines/new.json"], set()), [])

    def test_existing_file_needs_the_label(self):
        changed = ["bench/baselines/poisson_microbenchmark_gcc-release.json"]
        existed = set(changed)
        self.assertEqual(requiring_label(changed, existed), changed)


if __name__ == "__main__":
    unittest.main()
