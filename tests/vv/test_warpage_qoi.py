# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
# tools.vv is the intended V&V Python import surface for this ci-ok test.
"""PP-1: analytic self-check of tools.vv.warpage_qoi (reference values, not measured)."""
from __future__ import annotations

import math
import unittest

import numpy as np

from tools.vv.warpage_qoi import curvature_along, mirror_quarter, qoi_surface


class WarpageQoiAnalyticTest(unittest.TestCase):
    def setUp(self):
        self.k = 1.2e-3
        self.L = 40.0  # mm
        self.xs = self.ys = np.linspace(-self.L / 2, self.L / 2, 81)
        self.X, self.Y = np.meshgrid(self.xs, self.ys, indexing="ij")
        self.W = -self.k * (self.X**2 + self.Y**2) / 2 + 1e-3 * self.X - 2e-3 * self.Y + 0.05
        self.ref = self.k * self.L**2 / 4  # 参考值 (analytic, not measured)

    def _rel(self, got, ref):
        return abs(got / ref - 1.0)

    def _diagonal_kappa(self, W):
        # Same sampling as warpage_qoi.py __main__: diagonal (i,i), s = xs * sqrt(2), smax=20 mm.
        wd = np.array([W[i, i] for i in range(len(self.xs))])
        s = self.xs * math.sqrt(2)
        return curvature_along(s, wd, 20.0)

    def test_full_model_sphere(self):
        q = qoi_surface(self.X, self.Y, self.W)
        self.assertLessEqual(self._rel(q["W_bfp"], self.ref), 1e-12)
        self.assertLessEqual(self._rel(q["S_cc"], self.ref), 1e-12)
        kappa = self._diagonal_kappa(self.W)
        self.assertLessEqual(self._rel(kappa, self.k), 1e-12)

    def test_mirror_quarter_reconstruction(self):
        # mirror_quarter is a symmetry expander: uz is copied even, ux/uy flip with x/y.
        # The rigid tilt in __main__ is odd in x and y, so it is not a valid quarter-model
        # input. Reconstruct the even spherical field, then add the same tilt to the
        # full grid (best-fit plane removes it). This is the quarter-to-full check.
        Ws = -self.k * (self.X**2 + self.Y**2) / 2
        mask = (self.X >= 0) & (self.Y >= 0)
        P = np.column_stack(
            [
                self.X[mask],
                self.Y[mask],
                np.zeros(mask.sum()),
                np.zeros(mask.sum()),
                np.zeros(mask.sum()),
                Ws[mask],
            ]
        )
        Q = mirror_quarter(P)
        Wg = np.full_like(self.W, np.nan)
        coords = np.round(Q[:, :2], 9)
        for (x, y), uz in zip(coords, Q[:, 5]):
            ix = int(np.argmin(np.abs(self.xs - x)))
            iy = int(np.argmin(np.abs(self.ys - y)))
            if abs(self.xs[ix] - x) <= 5e-9 and abs(self.ys[iy] - y) <= 5e-9:
                Wg[ix, iy] = uz
        self.assertFalse(np.isnan(Wg).any(), "mirror_quarter did not cover the full 81x81 grid")
        Wg = Wg + 1e-3 * self.X - 2e-3 * self.Y + 0.05
        q = qoi_surface(self.X, self.Y, Wg)
        self.assertLessEqual(self._rel(q["W_bfp"], self.ref), 1e-12)
        self.assertLessEqual(self._rel(q["S_cc"], self.ref), 1e-12)
        kappa = self._diagonal_kappa(Wg)
        self.assertLessEqual(self._rel(kappa, self.k), 1e-12)


if __name__ == "__main__":
    unittest.main()
