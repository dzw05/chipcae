# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
"""Compare the mirrored even sphere with an independent full analytic grid."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(sys.argv.pop(1)).resolve()))

from qoi_parse import L, SAMPLE_COUNT, _rel, k, np, sorted_rows
from tools.vv.warpage_qoi import mirror_quarter, qoi_surface


class SurfaceTest(unittest.TestCase):
    def test_surface(self):
        xs = ys = np.linspace(-L / 2, L / 2, SAMPLE_COUNT)
        X, Y = np.meshgrid(xs, ys, indexing="ij")
        W = -k * (X**2 + Y**2) / 2
        mask = (X >= 0) & (Y >= 0)
        quarter = np.column_stack((X[mask], Y[mask], np.zeros(mask.sum()),
                                   np.zeros(mask.sum()), np.zeros(mask.sum()), W[mask]))
        self.assertTrue(np.array_equal(quarter[:, 5], W[mask]),
                        "Quarter input must contain only the even sphere, without rigid tilt")
        tilted = W + 1e-3 * X - 2e-3 * Y + 0.05
        self.assertFalse(np.array_equal(quarter[:, 5], tilted[mask]),
                         "The existing tilted fixture is not a quarter-symmetry input")
        actual = sorted_rows(mirror_quarter(quarter))
        expected = sorted_rows(np.column_stack((X.ravel(), Y.ravel(), np.zeros(X.size),
                                                np.zeros(X.size), np.zeros(X.size), W.ravel())))
        self.assertEqual(actual.shape, expected.shape)
        self.assertLessEqual(np.max(np.abs(actual[:, :3] - expected[:, :3])), 1e-12)
        self.assertLessEqual(np.max(np.abs(actual[:, 3:] - expected[:, 3:])), 1e-12)
        # Lexicographic coordinate sorting puts y along the inner grid dimension.
        reconstructed = actual[:, 5].reshape(X.shape)
        got, reference = qoi_surface(X, Y, reconstructed), qoi_surface(X, Y, W)
        for name in ("W_bfp", "S_cc"):
            with self.subTest(qoi=name):
                if reference[name] == 0:
                    self.assertLessEqual(abs(got[name] - reference[name]), 1e-12)
                else:
                    self.assertLessEqual(_rel(got[name], reference[name]), 1e-10)


if __name__ == "__main__":
    unittest.main()
