# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
"""Empty and repeated exact-axis inputs, determinism, and input preservation."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(sys.argv.pop(1)).resolve()))

from qoi_parse import np, sorted_rows
from tools.vv.warpage_qoi import mirror_quarter


class EmptyTest(unittest.TestCase):
    def test_empty(self):
        empty = np.empty((0, 6))
        before = empty.copy()
        first, second = mirror_quarter(empty), mirror_quarter(empty)
        self.assertEqual(first.shape, (0, 6))
        np.testing.assert_array_equal(first, second)
        np.testing.assert_array_equal(empty, before)

    def test_repeated_axes(self):
        points = np.array([[0., 0., 0., 0., 0., 0.],
                           [1., 0., 0., 2., 0., 3.],
                           [0., 2., 0., 0., 4., 5.]])
        repeated = np.repeat(points, 3, axis=0)
        before = repeated.copy()
        first = sorted_rows(mirror_quarter(repeated))
        second = sorted_rows(mirror_quarter(repeated))
        # Origin once, plus two reflected points for each nonzero axis point.
        self.assertEqual(len(first), 1 + 2 + 2)
        self.assertEqual(len(np.unique(first[:, :3], axis=0)), len(first))
        expected = sorted_rows(np.array([[0., 0., 0., 0., 0., 0.],
                                        [1., 0., 0., 2., 0., 3.],
                                        [-1., 0., 0., -2., 0., 3.],
                                        [0., 2., 0., 0., 4., 5.],
                                        [0., -2., 0., 0., -4., 5.]]))
        self.assertLessEqual(np.max(np.abs(first - expected)), 1e-12)
        np.testing.assert_array_equal(first, second)
        np.testing.assert_array_equal(repeated, before)


if __name__ == "__main__":
    unittest.main()
