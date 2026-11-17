# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
"""Analytic kinematic symmetry checks; all lengths are mm."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(sys.argv.pop(1)).resolve()))

from qoi_parse import np, sorted_rows
from tools.vv.warpage_qoi import mirror_quarter


def field(xs, ys):
    X, Y = np.meshgrid(xs, ys, indexing="ij")
    x, y = X.ravel(), Y.ravel()
    return np.column_stack((x, y, np.zeros(x.size),
                            x * (1 + y**2), y * (2 + x**2), x**2 + 3 * y**2))


class AxesTest(unittest.TestCase):
    def test_axes(self):
        xs, ys = np.arange(4.0), np.arange(3.0)
        quarter = field(xs, ys)
        full_x = np.arange(-3.0, 4.0)
        full_y = np.arange(-2.0, 3.0)
        expected = sorted_rows(field(full_x, full_y))
        actual = sorted_rows(mirror_quarter(quarter))
        self.assertEqual(len(actual), (2 * len(xs) - 1) * (2 * len(ys) - 1))
        self.assertEqual(len(np.unique(actual[:, :3], axis=0)), len(actual))
        self.assertLessEqual(np.max(np.abs(actual[:, :3] - expected[:, :3])), 1e-12)
        self.assertLessEqual(np.max(np.abs(actual[:, 3:] - expected[:, 3:])), 1e-12)
        for column, component in enumerate(("ux", "uy", "uz"), start=3):
            with self.subTest(component=component):
                self.assertLessEqual(
                    np.max(np.abs(actual[:, column] - expected[:, column])), 1e-12
                )
        values = {tuple(row[:3]): row[3:] for row in actual}
        for (x, y, z), u in values.items():
            across_x = values[(-x, y, z)]
            across_y = values[(x, -y, z)]
            self.assertLessEqual(abs(u[0] + across_x[0]), 1e-12)
            self.assertLessEqual(abs(u[1] + across_y[1]), 1e-12)
            self.assertLessEqual(abs(u[2] - across_x[2]), 1e-12)
            self.assertLessEqual(abs(u[2] - across_y[2]), 1e-12)


if __name__ == "__main__":
    unittest.main()
