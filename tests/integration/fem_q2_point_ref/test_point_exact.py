# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
"""Catch vertex interpolation, lost tensor terms and assumptions about MFEM ordering."""
from dataclasses import FrozenInstanceError, fields, is_dataclass
import sys
import unittest

import numpy as np

from q2_synth import (X, assert_field, bending, cubic, elements, general,
                      sample_points, temperature)
from tools.vv import fem_q2_point_ref as ref


class PointExactTests(unittest.TestCase):
    def test_mesh_contract_and_tensor_storage(self):
        data = elements(field=general)
        mesh = ref.q2_mesh_from_elements(data)
        self.assertIsInstance(mesh, ref.Q2Mesh)
        self.assertTrue(is_dataclass(mesh))
        self.assertEqual([field.name for field in fields(mesh)], ["gid", "lo", "hi", "U", "T"])
        with self.assertRaises(FrozenInstanceError):
            mesh.gid = np.array([0], dtype=np.int64)
        self.assertEqual(mesh.gid.shape, (12,))
        self.assertEqual(mesh.gid.dtype, np.dtype("int64"))
        for name, shape in (("lo", (12, 3)), ("hi", (12, 3)),
                            ("U", (12, 3, 3, 3, 3)), ("T", (12, 3, 3, 3))):
            value = getattr(mesh, name)
            self.assertEqual(value.shape, shape)
            self.assertEqual(value.dtype, np.dtype("float64"))
        self.assertTrue(np.all(np.diff(mesh.gid) > 0))
        for row, element in enumerate(sorted(data, key=lambda item: item["gid"])):
            points = np.asarray(element["points"])
            np.testing.assert_array_equal(mesh.lo[row], np.min(points, axis=0))
            np.testing.assert_array_equal(mesh.hi[row], np.max(points, axis=0))
            axes = [np.unique(points[:, axis]) for axis in range(3)]
            for node, point in enumerate(points):
                i, j, k = [int(np.flatnonzero(axis == coordinate)[0])
                           for axis, coordinate in zip(axes, point)]
                np.testing.assert_array_equal(mesh.U[row, i, j, k], element["U"][node])
                np.testing.assert_array_equal(mesh.T[row, i, j, k], element["T"][node])

    def test_bending_and_general_fields_are_exact(self):
        points = sample_points()
        for analytic in (bending, general):
            with self.subTest(field=analytic.__name__):
                mesh = ref.q2_mesh_from_elements(elements(field=analytic))
                assert_field(self, ref.fem_q2_point_values(mesh, points), analytic(points))
                assert_field(self, ref.fem_q2_point_values(mesh, points, field="U"), analytic(points))
                assert_field(self, ref.fem_q2_point_values(mesh, points, field="T"), temperature(points))
                check = ref.node_selfcheck(mesh)
                self.assertIsInstance(check, float)
                self.assertTrue(np.isfinite(check))
                self.assertGreaterEqual(check, 0)
                self.assertLessEqual(check, 1e-12)

    def test_cubic_matches_analytic_q2_and_differs_from_original_cubic(self):
        points = sample_points()
        mesh = ref.q2_mesh_from_elements(elements(field=cubic))
        x = points[:, 0]
        interval = np.clip(np.searchsorted(X, x, side="right")-1, 0, len(X)-2)
        a, b = X[interval], X[interval+1]
        # x^3 - its nodal Q2 polynomial is the monic cubic with roots a, midpoint, b.
        q2_x = x**3 - (x-a)*(x-(a+b)/2)*(x-b)
        expected = np.column_stack((q2_x, np.zeros_like(x), np.zeros_like(x)))
        actual = ref.fem_q2_point_values(mesh, points)
        assert_field(self, actual, expected)
        self.assertGreater(float(np.max(np.abs(actual[:500, 0]-x[:500]**3))),
                           1e-6 * float(np.max(np.abs(x[:500]**3))))

    def test_shared_faces_from_each_adjacent_cell(self):
        data = elements(field=general)
        boxes = [(element, np.min(element["points"], axis=0), np.max(element["points"], axis=0))
                 for element in data]
        pairs = 0
        for index, (left, lo, hi) in enumerate(boxes):
            for right, other_lo, other_hi in boxes[index+1:]:
                for axis in range(3):
                    if hi[axis] == other_lo[axis]:
                        lower, upper = np.maximum(lo, other_lo), np.minimum(hi, other_hi)
                    elif other_hi[axis] == lo[axis]:
                        lower, upper = np.maximum(lo, other_lo), np.minimum(hi, other_hi)
                    else:
                        continue
                    others = [direction for direction in range(3) if direction != axis]
                    if not np.all(upper[others] > lower[others]):
                        continue
                    pairs += 1
                    face = np.array([lower + [s, t, u]*(upper-lower)
                                     for s in (0, 0.23, 1) for t in (0, 0.61, 1)
                                     for u in (0, 0.37, 1)])
                    first = ref.q2_mesh_from_elements([left])
                    second = ref.q2_mesh_from_elements([right])
                    for field, analytic in (("U", general), ("T", temperature)):
                        a = ref.fem_q2_point_values(first, face, field)
                        b = ref.fem_q2_point_values(second, face, field)
                        assert_field(self, a, analytic(face))
                        assert_field(self, b, analytic(face))
                        self.assertLessEqual(float(np.max(np.abs(a-b))),
                                             1e-12 * float(np.max(np.abs(analytic(face)))))
        self.assertEqual(pairs, 20)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
