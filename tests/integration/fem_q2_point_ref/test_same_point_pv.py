# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
"""Analytic strip PV, sample order, all-cell counts and the selfcheck rejection boundary."""
import math
import sys
import unittest
from unittest.mock import patch

import numpy as np

from q2_synth import (PV_KEYS, analytic_pv, assert_bits, assert_prefix, centroids,
                      elements, strip_elements, strip_field)
from tools.vv import fem_q2_point_ref as ref


class SamePointPVTests(unittest.TestCase):
    def assert_result(self, result, xy, count):
        self.assertEqual(set(result), PV_KEYS)
        self.assertIs(type(result["pv"]), float)
        expected = analytic_pv(xy)
        self.assertLessEqual(abs(result["pv"]-expected), 1e-12*abs(expected))
        self.assertIs(type(result["n_points"]), int)
        self.assertIs(type(result["n_elements"]), int)
        self.assertEqual(result["n_points"], len(xy))
        self.assertEqual(result["n_elements"], count)
        self.assertIs(type(result["selfcheck_rel"]), float)
        self.assertTrue(math.isfinite(result["selfcheck_rel"]))
        self.assertGreaterEqual(result["selfcheck_rel"], 0)
        self.assertLessEqual(result["selfcheck_rel"], 1e-12)
        self.assertIs(type(result["eval_seconds"]), float)
        self.assertTrue(math.isfinite(result["eval_seconds"]))
        self.assertGreaterEqual(result["eval_seconds"], 0)

    def test_strip_pv_at_both_sampling_densities(self):
        mesh = ref.q2_mesh_from_elements(strip_elements())
        for nx, ny in ((12, 4), (48, 16)):
            with self.subTest(nx=nx, ny=ny):
                xy = centroids(0, 0, 1, 0.2, nx, ny)
                actual_xy = ref.uniform_top_centroids(0, 0, 1, 0.2, nx, ny)
                assert_bits(self, actual_xy, xy)
                w = strip_field(np.column_stack((xy, np.full(len(xy), 0.1))))[:, 2]
                # See analytic_pv's derivation: the fitted constant cancels in the PV.
                expected = analytic_pv(xy)
                self.assertLessEqual(abs(ref.plane_removed_pv(xy, w)-expected), 1e-12*expected)
                self.assert_result(ref.same_point_pv(mesh, xy, 0.1), xy, 48)

    def test_centroid_formula_with_translated_nonsquare_bbox(self):
        args = (-0.37, 0.021, 1.13, 0.193, 7, 3)
        assert_bits(self, ref.uniform_top_centroids(*args), centroids(*args))

    def test_uncovered_cells_still_contribute_to_total_count(self):
        # The second cell has no sampled x coordinates; the returned total is still two.
        mesh = ref.q2_mesh_from_elements(elements([0, 1, 2], [0, 0.2], [0, 0.1], strip_field))
        xy = centroids(0, 0, 1, 0.2, 12, 4)
        self.assert_result(ref.same_point_pv(mesh, xy, 0.1), xy, 2)

    def test_selfcheck_threshold_is_enforced_before_reporting_pv(self):
        mesh = ref.q2_mesh_from_elements(elements())
        xy = centroids(0, 0, 1, 0.2, 12, 4)
        # Only the scalar check result is injected; interpolation and PV remain real.
        with patch.object(ref, "node_selfcheck", return_value=np.nextafter(1e-12, np.inf)):
            assert_prefix(self, ref.Q2RefError, "Q2REF_SELFCHECK", ref.same_point_pv, mesh, xy, 0.1)
        with patch.object(ref, "node_selfcheck", return_value=1e-12):
            self.assert_result(ref.same_point_pv(mesh, xy, 0.1), xy, 12)

    def test_node_selfcheck_detects_temperature_discontinuity(self):
        data = elements([0, 0.5, 1], [0, 0.2], [0, 0.1])
        right = next(element for element in data if np.min(element["points"], axis=0)[0] > 0)
        right["T"] = (np.asarray(right["T"]) + 5).tolist()
        mesh = ref.q2_mesh_from_elements(data)
        self.assertGreater(ref.node_selfcheck(mesh), 1e-12)
        assert_prefix(self, ref.Q2RefError, "Q2REF_SELFCHECK", ref.same_point_pv,
                      mesh, centroids(0, 0, 1, 0.2, 12, 4), 0.1)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
