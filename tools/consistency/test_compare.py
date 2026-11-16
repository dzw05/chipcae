"""Synthetic CPU/GPU consistency cases. Bitwise equality is not required."""

from __future__ import annotations

import unittest

from tools.consistency.compare import (
    DISPLACEMENT_REL_MAX,
    LINEAR_RESIDUAL_MAX,
    NEWTON_TOLERANCE_MAX,
    STRESS_REL_MAX,
    WARPAGE_REL_MAX,
    compare_records,
)


def _side(**overrides):
    record = {
        "warpage": 1.0e-4,
        "displacement": [1.0e-4, -2.0e-4, 0.5e-4],
        "stress": [10.0, -4.0, 2.5, 0.0],
        "linear_relative_residual": 1.0e-14,
        "newton_tolerance": 1.0e-12,
    }
    record.update(overrides)
    return record


class ConsistencyTest(unittest.TestCase):
    def test_identical_passes_and_is_bitwise(self):
        report = compare_records(_side(), _side())
        self.assertTrue(report["ok"])
        self.assertTrue(report["bitwise_equal"])
        self.assertFalse(report["bitwise_required"])

    def test_within_tolerance_passes_without_bitwise_equality(self):
        scale = 1.0e-9
        gpu = _side(
            warpage=1.0e-4 * (1.0 + scale),
            displacement=[value * (1.0 + scale) for value in _side()["displacement"]],
            stress=[value * (1.0 + scale) if value else 0.0 for value in _side()["stress"]],
        )
        report = compare_records(_side(), gpu)
        self.assertFalse(report["bitwise_equal"])
        self.assertTrue(report["ok"])
        self.assertLess(report["metrics"]["warpage_rel"], WARPAGE_REL_MAX)
        self.assertLess(report["metrics"]["displacement_rel"], DISPLACEMENT_REL_MAX)
        self.assertLess(report["metrics"]["stress_rel"], STRESS_REL_MAX)

    def test_warpage_and_displacement_above_limit_block(self):
        gpu = _side(warpage=1.0e-4 * (1.0 + 1.0e-5))
        report = compare_records(_side(), gpu)
        self.assertFalse(report["ok"])
        self.assertTrue(report["blocks"])
        self.assertTrue(any("warpage" in item for item in report["failures"]))

        shifted = [value + 1.0e-8 for value in _side()["displacement"]]
        report = compare_records(_side(), _side(displacement=shifted))
        self.assertFalse(report["ok"])
        self.assertGreater(report["metrics"]["displacement_rel"], DISPLACEMENT_REL_MAX)

    def test_stress_uses_max_abs_scale(self):
        peak = max(abs(value) for value in _side()["stress"])
        allowed = [value + (STRESS_REL_MAX * peak if index == 0 else 0.0) for index, value in enumerate(_side()["stress"])]
        report = compare_records(_side(), _side(stress=allowed))
        self.assertTrue(report["ok"])
        self.assertLessEqual(report["metrics"]["stress_rel"], STRESS_REL_MAX + 1e-15)

        too_far = [value + (2 * STRESS_REL_MAX * peak if index == 0 else 0.0) for index, value in enumerate(_side()["stress"])]
        report = compare_records(_side(), _side(stress=too_far))
        self.assertFalse(report["ok"])
        self.assertGreater(report["metrics"]["stress_rel"], STRESS_REL_MAX)

    def test_direct_solver_zero_residual_passes(self):
        report = compare_records(
            _side(linear_relative_residual=0.0),
            _side(linear_relative_residual=0.0),
        )
        self.assertTrue(report["ok"])

    def test_linear_residual_above_limit_blocks_before_fields(self):
        report = compare_records(_side(), _side(linear_relative_residual=LINEAR_RESIDUAL_MAX * 2))
        self.assertFalse(report["solver_ok"])
        self.assertFalse(report["ok"])
        self.assertTrue(any("linear relative residual" in item for item in report["failures"]))

    def test_newton_tolerance_boundary(self):
        report = compare_records(
            _side(newton_tolerance=NEWTON_TOLERANCE_MAX),
            _side(newton_tolerance=NEWTON_TOLERANCE_MAX),
        )
        self.assertTrue(report["ok"])
        report = compare_records(_side(), _side(newton_tolerance=NEWTON_TOLERANCE_MAX * 10))
        self.assertFalse(report["ok"])
        self.assertTrue(any("Newton" in item for item in report["failures"]))

    def test_both_zero_warpage_passes(self):
        report = compare_records(_side(warpage=0.0), _side(warpage=0.0))
        self.assertTrue(report["ok"])
        self.assertEqual(report["metrics"]["warpage_rel"], 0.0)


if __name__ == "__main__":
    unittest.main()
