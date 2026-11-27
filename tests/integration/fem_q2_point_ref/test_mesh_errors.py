# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
"""Validation prefixes, tolerance boundaries and deterministic multi-rank loading."""
from copy import deepcopy
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

import numpy as np

from q2_synth import assert_bits, assert_prefix, centroids, elements, general, write_rank
from tools.vv import fem_q2_point_ref as ref


class MeshErrorTests(unittest.TestCase):
    def setUp(self):
        self.data = elements([0, 1], [0, 1], [0, 1], general)
        self.mesh = ref.q2_mesh_from_elements(self.data)

    def error(self, prefix, function, *args, **kwargs):
        assert_prefix(self, ref.Q2RefError, prefix, function, *args, **kwargs)

    def test_error_class_is_a_value_error(self):
        self.assertTrue(issubclass(ref.Q2RefError, ValueError))

    def test_empty_and_bad_element_structures(self):
        self.error("Q2REF_EMPTY", ref.q2_mesh_from_elements, [])
        for bad in (None, 42, "elements", [None], [3], [[]]):
            with self.subTest(bad=bad):
                self.error("Q2REF_BAD_JSON", ref.q2_mesh_from_elements, bad)
        for key in ("gid", "points", "U", "T"):
            data = deepcopy(self.data)
            del data[0][key]
            with self.subTest(missing=key):
                self.error("Q2REF_BAD_JSON", ref.q2_mesh_from_elements, data)
        for gid in ("not-an-int", 1.25, 2**63, -(2**63)-1, None):
            data = deepcopy(self.data)
            data[0]["gid"] = gid
            with self.subTest(gid=gid):
                self.error("Q2REF_BAD_JSON", ref.q2_mesh_from_elements, data)
        for key in ("points", "U", "T"):
            data = deepcopy(self.data)
            data[0][key] = ["not-a-number"] * 27
            with self.subTest(nonnumeric=key):
                self.error("Q2REF_BAD_JSON", ref.q2_mesh_from_elements, data)

    def test_node_count_lengths_and_shapes(self):
        cases = [("points", np.zeros((26, 3)).tolist()),
                 ("points", np.zeros((28, 3)).tolist()),
                 ("points", np.zeros((27, 2)).tolist()),
                 ("points", np.zeros((27, 3, 1)).tolist()),
                 ("U", np.zeros((26, 3)).tolist()),
                 ("U", np.zeros((27, 2)).tolist()),
                 ("U", np.zeros(27).tolist()),
                 ("T", np.zeros(26).tolist()),
                 ("T", np.zeros((27, 1)).tolist()), ("T", 2.0)]
        for key, value in cases:
            with self.subTest(key=key, shape=np.shape(value)):
                data = deepcopy(self.data)
                data[0][key] = value
                self.error("Q2REF_NODE_COUNT", ref.q2_mesh_from_elements, data)
        data = deepcopy(self.data)
        data[0]["U"][0] = [1, 2]
        self.error("Q2REF_BAD_JSON", ref.q2_mesh_from_elements, data)

    def test_nonfinite_nodes_fields_and_query_points(self):
        for key in ("points", "U", "T"):
            for value in (float("nan"), float("inf"), -float("inf")):
                with self.subTest(key=key, value=value):
                    data = deepcopy(self.data)
                    if key == "T":
                        data[0][key][0] = value
                    else:
                        data[0][key][0][0] = value
                    self.error("Q2REF_NONFINITE", ref.q2_mesh_from_elements, data)
        for axis in range(3):
            for value in (float("nan"), float("inf"), -float("inf")):
                xyz = np.array([[0.2, 0.3, 0.4]])
                xyz[0, axis] = value
                self.error("Q2REF_NONFINITE", ref.fem_q2_point_values, self.mesh, xyz)

    def test_duplicate_ids_and_nodes(self):
        self.error("Q2REF_DUP_GID", ref.q2_mesh_from_elements, self.data + deepcopy(self.data))
        data = deepcopy(self.data)
        data[0]["points"][0] = data[0]["points"][1][:]
        self.error("Q2REF_DUP_NODE", ref.q2_mesh_from_elements, data)

    def test_degenerate_skewed_and_nonmidpoint_boxes(self):
        for kind in ("zero_span", "tiny_span", "skew", "four_levels", "nonmidpoint"):
            with self.subTest(kind=kind):
                data = deepcopy(self.data)
                points = np.array(data[0]["points"])
                if kind == "zero_span":
                    points[:, 0] = 0.5
                elif kind == "tiny_span":
                    points[:, 0] *= 1e-13
                elif kind == "skew":
                    points[:, 0] += 0.1 * points[:, 1]
                elif kind == "four_levels":
                    points[0, 0] += 0.1
                else:
                    points[points[:, 0] == 0.5, 0] = 0.51
                data[0]["points"] = points.tolist()
                self.error("Q2REF_NOT_AXIS_ALIGNED", ref.q2_mesh_from_elements, data)

    def test_coordinate_merge_and_midpoint_tolerance_on_each_side(self):
        for shift, accepted in ((0.25e-12, True), (4e-12, False)):
            for kind in ("repeated_level", "midpoint"):
                with self.subTest(shift=shift, kind=kind):
                    data = deepcopy(self.data)
                    points = np.array(data[0]["points"])
                    if kind == "repeated_level":
                        node = np.flatnonzero(points[:, 0] == 0.5)[0]
                        points[node, 0] += shift
                    else:
                        points[points[:, 0] == 0.5, 0] += shift
                    data[0]["points"] = points.tolist()
                    if accepted:
                        mesh = ref.q2_mesh_from_elements(data)
                        self.assertEqual(mesh.gid.shape, (1,))
                    else:
                        self.error("Q2REF_NOT_AXIS_ALIGNED", ref.q2_mesh_from_elements, data)

    def test_cell_tolerance_applies_per_axis(self):
        # A single anisotropic cell prevents a global-length tolerance from passing.
        mesh = ref.q2_mesh_from_elements(elements([0, 1], [0, 0.2], [0, 0.1], general))
        lengths = np.array([1, 0.2, 0.1])
        for axis in range(3):
            for upper in (False, True):
                xyz = np.array([[0.5, 0.1, 0.05]])
                boundary = lengths[axis] if upper else 0
                direction = 1 if upper else -1
                xyz[0, axis] = boundary + direction * 0.5e-12 * lengths[axis]
                self.assertTrue(np.all(np.isfinite(ref.fem_q2_point_values(mesh, xyz))))
                xyz[0, axis] = boundary + direction * 2e-12 * lengths[axis]
                self.error("Q2REF_POINT_OUTSIDE", ref.fem_q2_point_values, mesh, xyz)

    def test_reports_first_outside_point_in_input_order(self):
        points = np.array([[0.1, 0.2, 0.3], [1.25, 2.25, 3.25], [-9.5, -8.5, -7.5]])
        with self.assertRaises(ref.Q2RefError) as caught:
            ref.fem_q2_point_values(self.mesh, points)
        message = str(caught.exception)
        self.assertTrue(message.startswith("Q2REF_POINT_OUTSIDE"), message)
        for coordinate in ("1.25", "2.25", "3.25"):
            self.assertIn(coordinate, message)
        self.assertNotIn("-9.5", message)

    def test_query_field_and_shape_errors(self):
        for field in ("u", "t", "temperature", "", None):
            self.error("Q2REF_BAD_ARG", ref.fem_q2_point_values, self.mesh,
                       np.array([[0.2, 0.3, 0.4]]), field=field)
        for xyz in (np.zeros(3), np.zeros((2, 2)), np.zeros((2, 3, 1)), [["bad", 0, 0]]):
            self.error("Q2REF_BAD_ARG", ref.fem_q2_point_values, self.mesh, xyz)

    def test_validation_order_shape_before_finiteness_before_range(self):
        data = deepcopy(self.data)
        data[0]["U"] = np.full((26, 3), np.nan).tolist()
        self.error("Q2REF_NODE_COUNT", ref.q2_mesh_from_elements, data)
        data = deepcopy(self.data)
        data[0]["points"] = np.zeros((27, 3)).tolist()
        data[0]["T"][0] = float("nan")
        self.error("Q2REF_NONFINITE", ref.q2_mesh_from_elements, data)
        self.error("Q2REF_BAD_ARG", ref.fem_q2_point_values, self.mesh, np.full((1, 2), np.nan))
        self.error("Q2REF_NONFINITE", ref.fem_q2_point_values, self.mesh, [[4, 5, float("nan")]])
        self.error("Q2REF_BAD_ARG", ref.same_point_pv, self.mesh, np.full((2, 3), np.nan), 1.0)
        self.error("Q2REF_NONFINITE", ref.uniform_top_centroids, 0, 0, 1, float("inf"), 0, 1)

    def test_centroid_and_pv_bad_args_and_nonfinite_inputs(self):
        for nx, ny in ((0, 2), (-1, 2), (2, 0), (2, -1), (1.5, 2), (2, "bad")):
            self.error("Q2REF_BAD_ARG", ref.uniform_top_centroids, 0, 0, 1, 1, nx, ny)
        for bbox in ((0, 0, 0, 1), (1, 0, 0, 1), (0, 0, 1, 0), (0, 1, 1, 0)):
            self.error("Q2REF_BAD_ARG", ref.uniform_top_centroids, *bbox, 2, 2)
        for index in range(4):
            bbox = [0, 0, 1, 1]
            bbox[index] = float("nan")
            self.error("Q2REF_NONFINITE", ref.uniform_top_centroids, *bbox, 2, 2)
        valid_xy = centroids(0, 0, 1, 1, 4, 2)
        for xy in (np.empty((0, 2)), np.zeros((3, 3)), np.zeros(2), [["bad", 0]]):
            self.error("Q2REF_BAD_ARG", ref.same_point_pv, self.mesh, xy, 1.0)
            self.error("Q2REF_BAD_ARG", ref.plane_removed_pv, xy, np.zeros(len(xy)))
        for value in (float("nan"), float("inf"), -float("inf")):
            self.error("Q2REF_NONFINITE", ref.same_point_pv, self.mesh, valid_xy, value)
            bad_xy = valid_xy.copy()
            bad_xy[0, 0] = value
            self.error("Q2REF_NONFINITE", ref.same_point_pv, self.mesh, bad_xy, 1.0)
            self.error("Q2REF_NONFINITE", ref.plane_removed_pv, bad_xy, np.zeros(len(bad_xy)))
            w = np.zeros(len(valid_xy))
            w[0] = value
            self.error("Q2REF_NONFINITE", ref.plane_removed_pv, valid_xy, w)
        for w in (np.zeros(len(valid_xy)-1), np.zeros((len(valid_xy), 1))):
            self.error("Q2REF_BAD_ARG", ref.plane_removed_pv, valid_xy, w)

    def test_loader_empty_bad_json_missing_keys_and_io_path(self):
        with tempfile.TemporaryDirectory(prefix="q2ref-errors-") as tmp:
            run = Path(tmp)
            self.error("Q2REF_NO_FILES", ref.load_fem_q2, run)
            self.error("Q2REF_NO_FILES", ref.load_fem_q2, run / "absent")
            path = run / "accuracy-q2-rank0.json"
            for contents, prefix in (("{broken", "Q2REF_BAD_JSON"),
                                     ("", "Q2REF_BAD_JSON"),
                                     ('{"elements": []}', "Q2REF_EMPTY"),
                                     ("{}", "Q2REF_EMPTY"),
                                     ('{"elements": [null]}', "Q2REF_BAD_JSON"),
                                     ('{"elements": "bad"}', "Q2REF_BAD_JSON")):
                path.write_text(contents, encoding="utf-8")
                self.error(prefix, ref.load_fem_q2, run)
            # A directory with the expected filename is a portable read error, even as root.
            path.unlink()
            path.mkdir()
            with self.assertRaises(ref.Q2RefError) as caught:
                ref.load_fem_q2(run)
            self.assertTrue(str(caught.exception).startswith("Q2REF_"))
            self.assertIn(str(path), str(caught.exception))

    def test_duplicate_gid_across_rank_files_and_empty_rank_rejection(self):
        with tempfile.TemporaryDirectory(prefix="q2ref-ranks-") as tmp:
            run = Path(tmp)
            write_rank(run, 9, self.data)
            write_rank(run, 10, self.data)
            self.error("Q2REF_DUP_GID", ref.load_fem_q2, run)
            write_rank(run, 10, [])
            self.error("Q2REF_EMPTY", ref.load_fem_q2, run)

    def test_numeric_gid_sort_is_bitwise_stable_across_rank_and_element_order(self):
        data = elements(field=general)
        # Include negative, int64-extreme and >2**53 ids to catch float conversion/truncation.
        gids = [-(2**63), -17, -2, 0, 1, 2, 9, 10, 100, 2**53+1, 2**53+2, 2**63-1]
        for element, gid in zip(data, gids):
            element["gid"] = gid
        direct = ref.q2_mesh_from_elements(data)
        permuted = ref.q2_mesh_from_elements(list(reversed(data)))
        np.testing.assert_array_equal(direct.gid, np.array(gids, dtype=np.int64))
        with tempfile.TemporaryDirectory(prefix="q2ref-order-") as tmp:
            run = Path(tmp)
            write_rank(run, 10, list(reversed(data[::2])))
            write_rank(run, 9, data[1::2])
            loaded = ref.load_fem_q2(run)
            write_rank(run, 10, data[::2])
            write_rank(run, 9, list(reversed(data[1::2])))
            # Reverse discovery, not JSON parsing; leave file I/O real.
            original_glob = Path.glob
            def reverse_glob(path, pattern, *args, **kwargs):
                return iter(reversed(list(original_glob(path, pattern, *args, **kwargs))))
            with patch.object(Path, "glob", reverse_glob):
                reordered = ref.load_fem_q2(run)
            for candidate in (permuted, loaded, reordered):
                self.assertTrue(np.all(candidate.gid[1:] > candidate.gid[:-1]))
                for name in ("gid", "lo", "hi", "U", "T"):
                    assert_bits(self, getattr(candidate, name), getattr(direct, name))
            xy = centroids(0, 0, 1, 0.2, 12, 4)
            pvs = [ref.same_point_pv(mesh, xy, 0.1)["pv"] for mesh in (direct, loaded, reordered)]
            self.assertEqual([float(value).hex() for value in pvs], [float(pvs[0]).hex()] * 3)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
