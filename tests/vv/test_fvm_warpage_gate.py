# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
"""Analytic acceptance for same-point warpage gates; never launches a solver."""

import importlib
import json
import math
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

try:
    import h5py
    import numpy as np
except ModuleNotFoundError as exc:
    if exc.name not in ("numpy", "h5py"):
        raise
    print(f"missing optional dependency: {exc.name}")
    raise SystemExit(77)

from tools.vv.field_compare import InputInvalid, load_run
from tests.vv.fvm_warpage_gate_a import support
from tests.vv.fvm_warpage_gate_a.synthetic import (
    METHOD, change_manifest, cut_run, grid_run,
)

ROOT = Path(__file__).resolve().parents[2]


def gate_module():
    assert (ROOT / "tools/vv/fvm_warpage_gate.py").is_file(), (
        "missing feature: tools/vv/fvm_warpage_gate.py (same-point scheme A gate library)")
    return importlib.import_module("tools.vv.fvm_warpage_gate")


def records():
    # Both kz order log2(3.5)>1.5 and retained in-plane order log2(2)=1.
    return [dict(nx=nx, ny=ny, kz=kz, same_point_rel=error,
                 interp_indicator=0.0, deformation_rel=0.0,
                 fvm_curvature=-.2, fem_curvature=-.3)
            for nx, ny, e4, e8 in ((48, 16, .028, .008), (96, 32, .014, .004))
            for kz, error in ((4, e4), (8, e8))]


class GateTests(unittest.TestCase):
    def setUp(self):
        self.gate = gate_module()

    def judge(self, items):
        result = self.gate.scheme_a_gate(items)
        self.assertTrue({"pass", "checks"} <= result.keys())
        self.assertIs(type(result["pass"]), bool)
        self.assertIsInstance(result["checks"], list)
        self.assertTrue(result["checks"], "a gate cannot pass without judging any criteria")
        for check in result["checks"]:
            self.assertTrue({"name", "value", "limit", "pass", "reason"} <= check.keys())
            self.assertIs(type(check["pass"]), bool)
            if not check["pass"]:
                self.assertTrue(check["reason"])
        self.assertEqual(result["pass"], all(c["pass"] for c in result["checks"]))
        return result

    def reason(self, result, phrase):
        self.assertFalse(result["pass"])
        self.assertIn(phrase, " ".join(c["reason"] for c in result["checks"] if not c["pass"]))

    def test_kz8_pv_boundaries_are_inclusive_without_acceptance_epsilon(self):
        for sign in (-1, 1):
            for nx, limit, above in ((48, .010, .0100001), (96, .005, .0050001)):
                for value, expected in ((limit, True), (above, False)):
                    with self.subTest(sign=sign, nx=nx, value=value):
                        items = records()
                        for item in items:
                            item["same_point_rel"] = sign * (
                                (.03 if item["nx"] == 48 else .015) if item["kz"] == 4
                                else (.01 if item["nx"] == 48 else .005))
                            if item["nx"] == nx and item["kz"] == 8:
                                item["same_point_rel"] = sign * value
                        self.assertEqual(self.judge(items)["pass"], expected)

    def test_thickness_order_boundary(self):
        for order, expected in ((1.5, True), (1.4999, False)):
            with self.subTest(order=order):
                items = records()
                for item in items:
                    e8 = .008 if item["nx"] == 48 else .004
                    item["same_point_rel"] = e8 * (2**order if item["kz"] == 4 else 1)
                self.assertEqual(self.judge(items)["pass"], expected)

    def test_missing_kz8_in_either_required_grid_fails(self):
        for nx in (48, 96):
            with self.subTest(nx=nx):
                items = [r for r in records() if not (r["nx"] == nx and r["kz"] == 8)]
                self.reason(self.judge(items), "缺 kz≥8 运行")

    def test_kz4_original_gate_cannot_be_replaced_by_a_passing_kz8(self):
        items = records()
        items[2]["same_point_rel"] = .0151
        self.assertFalse(self.judge(items)["pass"])

    def test_nx_at_least_96_keeps_the_same_pv_limits(self):
        extra = [{**item, "nx": 192, "ny": 64, "same_point_rel": item["same_point_rel"] / 2}
                 for item in records() if item["nx"] == 96]
        self.assertTrue(self.judge([*records(), *extra])["pass"])
        extra[1]["same_point_rel"] = .0050001
        self.assertFalse(self.judge([*records(), *extra])["pass"])

    def test_sign_reversal_small_coarse_and_nondecaying_errors_have_no_order(self):
        for e4, e8 in ((.014, -.004), (.001, .0001), (.0009, .0001),
                       (.004, .004), (.003, .004), (.014, 0.0)):
            with self.subTest(e4=e4, e8=e8):
                items = records()
                items[2]["same_point_rel"], items[3]["same_point_rel"] = e4, e8
                self.reason(self.judge(items), "阶不可判")

    def test_kz16_is_diagnostic_even_when_its_values_exceed_limits(self):
        base = self.judge(records())
        diagnostic = dict(nx=96, ny=32, kz=16, same_point_rel=.9,
                          interp_indicator=.9, deformation_rel=.9)
        self.assertEqual(self.judge([*records(), diagnostic])["pass"], base["pass"])

    def test_deformation_and_interpolation_boundaries(self):
        for index, tau in ((0, .03), (1, .010), (2, .015), (3, .005)):
            for key, limit in (("deformation_rel", .05),
                               ("interp_indicator", min(tau / 10, 1e-3))):
                for value, expected in ((limit, True), (math.nextafter(limit, math.inf), False)):
                    with self.subTest(index=index, key=key, value=value):
                        items = records()
                        items[index][key] = value
                        result = self.judge(items)
                        self.assertEqual(result["pass"], expected)
                        if key == "interp_indicator" and not expected:
                            self.reason(result, "参考插值不足")

    def test_checks_are_deterministic_under_record_permutation(self):
        self.assertEqual(self.judge(records()), self.judge(list(reversed(records()))))

    def test_curvature_direction_is_independent_of_error_sign(self):
        for fvm, fem, expected in ((.2, .3, True), (-.2, -.3, True),
                                   (.2, -.3, False), (-.2, .3, False)):
            items = records()
            items[0].update(fvm_curvature=fvm, fem_curvature=fem)
            result = self.judge(items)
            self.assertEqual(result["pass"], expected)
            if not expected:
                self.reason(result, "翘曲方向")

    def test_unresolved_curvature_direction_fails(self):
        for key in ("fvm_curvature", "fem_curvature"):
            for value in (None, 0., math.nan, math.inf, -math.inf, "unknown", True):
                with self.subTest(key=key, value=value):
                    items = records()
                    items[0][key] = value
                    self.reason(self.judge(items), "翘曲方向")
            items = records()
            del items[0][key]
            self.reason(self.judge(items), "翘曲方向")

    def test_duplicate_grid_thickness_records_are_rejected(self):
        with self.assertRaises(InputInvalid):
            self.gate.scheme_a_gate([*records(), records()[0]])

    def test_malformed_and_nonfinite_records_cannot_pass(self):
        for key in ("same_point_rel", "deformation_rel", "interp_indicator"):
            for value in (math.nan, math.inf, -math.inf):
                with self.subTest(key=key, value=value):
                    items = records()
                    items[0][key] = value
                    try:
                        result = self.gate.scheme_a_gate(items)
                    except InputInvalid:
                        continue
                    self.assertFalse(result["pass"])
        for items in ([{}], [{**records()[0], "nx": -48}]):
            with self.subTest(items=items), self.assertRaises(InputInvalid):
                self.gate.scheme_a_gate(items)
        self.assertFalse(self.gate.scheme_a_gate([])["pass"])

    def test_preconditions_preserve_each_caller_supplied_legacy_gate(self):
        for limit in (.05, .08, .10, .037):
            for kz, same, nodal, expected in (
                    (7, 0.0, 0.0, False), (8, limit, -limit, True),
                    (8, limit, math.nextafter(limit, math.inf), False),
                    (8, math.nextafter(limit, math.inf), limit, False),
                    (12, -limit, None, True)):
                with self.subTest(limit=limit, kz=kz, same=same, nodal=nodal):
                    result = self.gate.scheme_a_preconditions(96, kz, same, nodal, limit)
                    self.assertEqual(set(result), {"pass", "checks", "reason"})
                    self.assertEqual(result["pass"], expected)
                    checks = {check["name"]: check for check in result["checks"]}
                    names = {"kz_min>=8", "same_point_abs<=legacy_gate"}
                    if nodal is not None:
                        names.add("nodal_abs<=legacy_gate")
                    self.assertEqual(set(checks), names)
                    for check in checks.values():
                        self.assertEqual(set(check), {"name", "value", "limit", "pass"})
                    self.assertTrue(math.isclose(checks["same_point_abs<=legacy_gate"]["limit"],
                                                 limit, rel_tol=1e-15))
                    self.assertEqual(result["pass"], all(c["pass"] for c in checks.values()))
                    if not expected:
                        self.assertTrue(result["reason"])

    def test_preconditions_cannot_accept_nonfinite_errors_or_invalid_legacy_gate(self):
        for same, nodal, limit in ((math.nan, .01, .05), (.01, math.inf, .05),
                                   (.01, .01, math.nan), (.01, .01, -1)):
            with self.subTest(same=same, nodal=nodal, limit=limit):
                try:
                    result = self.gate.scheme_a_preconditions(96, 8, same, nodal, limit)
                except InputInvalid:
                    continue
                self.assertFalse(result["pass"])

    def test_only_warpage_and_deformation_are_convergence_quantities(self):
        for name in ("U_mag_max", "T_max", "unknown", "", None):
            with self.subTest(name=name), self.assertRaises(ValueError):
                self.gate.require_convergence_quantity(name)
        for name in ("warpage_pv", "deformation_max"):
            self.gate.require_convergence_quantity(name)


class AnalyticFieldTests(unittest.TestCase):
    def setUp(self):
        self.gate = gate_module()
        self.temp = tempfile.TemporaryDirectory(prefix="warpage-analytic-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def test_quadratic_pv_at_sampled_midpoint_and_pure_plane(self):
        xy = np.array([(x, y) for y in (-.5, 0, .5) for x in np.linspace(-1, 1, 25)])
        x, y = xy.T
        w = .125 + .25*x - .375*y + 1.5*x*x  # kappa=3, span=2, PV=1.5
        self.assertTrue(math.isclose(self.gate.plane_pv(xy, w), 1.5, rel_tol=1e-12))
        self.assertLessEqual(self.gate.plane_pv(xy, .125 + .25*x - .375*y), 1e-15)

    def test_plane_fit_is_stable_for_translated_and_anisotropic_coordinates(self):
        base = np.array([(x, y) for x in (-1, 0, 1) for y in (-1, 0, 1)], dtype=float)
        xy = base * [2**-20, 2**10] + [2**12, -2**30]
        w = base[:, 0]**2 + .125*base[:, 1]**2
        self.assertTrue(math.isclose(self.gate.plane_pv(xy, w), 1.125, rel_tol=1e-12))

    def test_discrete_centroid_node_quadratic_pv_ratios(self):
        # Analytic sampled extrema: even grids miss the centroid midpoint;
        # odd grids miss the nodal midpoint. Never insert a synthetic sample.
        for nx, expected in ((24, 1-2/24), (48, 1-2/48), (96, 1-2/96),
                              (192, 1-2/192), (25, 24/26)):
            with self.subTest(nx=nx):
                nodes = np.linspace(-1, 1, nx+1)
                centers = (nodes[:-1] + nodes[1:]) / 2
                nodal_xy = np.array([(x, y) for x in nodes for y in (-.5, .5)])
                center_xy = np.array([(x, y) for x in centers for y in (-.5, .5)])
                nodal = self.gate.plane_pv(nodal_xy, 1.5*nodal_xy[:, 0]**2)
                centroid = self.gate.plane_pv(center_xy, 1.5*center_xy[:, 0]**2)
                self.assertTrue(math.isclose(centroid/nodal, expected, rel_tol=1e-12))

    def test_plane_pv_rejects_malformed_samples(self):
        xy = np.array([[0., 0.], [1., 0.], [0., 1.], [1., 1.]])
        cases = [(xy[:2], np.ones(2)), (xy, np.ones(3)), (xy[:, :1], np.ones(4)),
                 (xy * math.nan, np.ones(4)), (xy, np.full(4, math.inf)),
                 (np.zeros((4, 2)), np.arange(4.))]
        for points, values in cases:
            with self.subTest(points=points, values=values), self.assertRaises(InputInvalid):
                self.gate.plane_pv(points, values)

    def test_bivariate_quadratic_interpolates_interior_and_one_sided_boundaries(self):
        def field(x, y):
            return .3 + .2*x - .1*y + 1.5*x*x + .7*x*y + .4*y*y
        run = grid_run(self.root / "fem", np.linspace(0, 1, 9), np.linspace(0, 1, 9), field)
        xy = np.array([(x, y) for x in (0, .001, .217, .51, .999, 1)
                       for y in (0, .13, .73, 1)])
        values, indicator = self.gate.fem_top_interpolate(run, xy)
        np.testing.assert_allclose(values, field(*xy.T), rtol=1e-12, atol=0)
        self.assertTrue(math.isfinite(indicator) and 0 <= indicator <= 1e-12)

    def test_cubic_indicator_is_positive_and_cubic_in_spacing(self):
        indicators = []
        for n in (16, 32):
            # Mid-cell phase stays fixed under refinement. Sampling the whole
            # domain also exercises one-sided stencils without a phase artifact.
            centers = (np.arange(n) + .5) / n
            xy = np.array([(x, y) for x in centers for y in centers])
            run = grid_run(self.root / f"fem{n}", np.linspace(0, 1, n+1),
                           np.linspace(0, 1, n+1),
                           lambda x, y: x*x + .2*y*y + .1*(x**3 + y**3))
            _, indicator = self.gate.fem_top_interpolate(run, xy)
            self.assertGreater(indicator, 0)
            indicators.append(indicator)
        self.assertGreaterEqual(indicators[0] / indicators[1], 6)
        self.assertLessEqual(indicators[0] / indicators[1], 10)

    def test_unavailable_stencils_incomplete_lattice_and_extrapolation_fail(self):
        xy = np.array([[.1, .2], [.4, .6], [.8, .7], [.5, .3]])
        small = grid_run(self.root / "small", [0, .5, 1], [0, .5, 1])
        with self.assertRaisesRegex(InputInvalid, "参考插值不足"):
            self.gate.fem_top_interpolate(small, xy)
        run = grid_run(self.root / "fem", np.linspace(0, 1, 5), np.linspace(0, 1, 5))
        with self.assertRaisesRegex(InputInvalid, "参考插值不足"):
            self.gate.fem_top_interpolate(run, xy + [1, 0])
        with h5py.File(run / "nested/first.vtkhdf", "r+") as data:
            points = data["VTKHDF/hex/Points"]
            points[-1] = [.9, 1, 1]  # valid topology, incomplete rectangular top lattice
        with self.assertRaisesRegex(InputInvalid, "参考插值不足"):
            self.gate.fem_top_interpolate(run, xy)

    def test_zero_reference_pv_cannot_define_an_interpolation_indicator(self):
        run = grid_run(self.root / "flat", np.linspace(0, 1, 5), np.linspace(0, 1, 5),
                       lambda x, y: np.zeros_like(x))
        with self.assertRaisesRegex(InputInvalid, "参考插值不足"):
            self.gate.fem_top_interpolate(run, np.array([[.1, .1], [.4, .8], [.8, .3]]))

    def test_same_point_errors_use_manifest_face_pv_and_keep_their_sign(self):
        fvm = grid_run(self.root / "fvm", np.linspace(-1, 1, 5), [-1, 0, 1],
                       lambda x, y: np.zeros_like(x), pv=.495)
        fem = grid_run(self.root / "fem", np.linspace(-1, 1, 9), np.linspace(-1, 1, 9),
                       lambda x, y: x*x + .3*y*y, pv=1.3, method="synthetic FEM")
        result = self.gate.same_point_error(fvm, fem)
        self.assertTrue({"fvm_pv", "fem_same_point_pv", "same_point_rel", "fem_nodal_pv",
                         "nodal_rel", "interp_indicator", "n_samples"} <= result.keys())
        self.assertEqual(result["n_samples"], 8)
        for name, value in (("fvm_pv", .495), ("fem_same_point_pv", .5),
                            ("same_point_rel", -.01), ("fem_nodal_pv", 1.3),
                            ("nodal_rel", .495 / 1.3 - 1)):
            self.assertTrue(math.isclose(result[name], value, rel_tol=1e-12), (name, result))
        self.assertLessEqual(result["interp_indicator"], 1e-12)

    def test_wrong_method_nonpositive_or_nonfinite_manifest_pvs_fail(self):
        for index, value in enumerate((0., -1., math.nan, math.inf)):
            for which in ("fvm", "fem"):
                with self.subTest(value=value, which=which):
                    fvm = grid_run(self.root / f"a{which}{index}", [-1, -.5, 0, .5, 1], [-1, 0, 1])
                    fem = grid_run(self.root / f"b{which}{index}", np.linspace(-1, 1, 9),
                                   np.linspace(-1, 1, 9))
                    change_manifest(fvm if which == "fvm" else fem,
                                    lambda m: m["summary"][0].update(value=value))
                    with self.assertRaises(InputInvalid):
                        self.gate.same_point_error(fvm, fem)
        fvm = grid_run(self.root / "badmethod", [-1, -.5, 0, .5, 1], [-1, 0, 1],
                       method=METHOD + " ")
        fem = grid_run(self.root / "valid-reference", np.linspace(-1, 1, 9),
                       np.linspace(-1, 1, 9), pv=1.0, method="synthetic FEM")
        with self.assertRaises(InputInvalid):
            self.gate.same_point_error(fvm, fem)

    def test_same_point_requires_case_request_independent_of_manifest(self):
        fem = grid_run(self.root / "reference", np.linspace(-1, 1, 9),
                       np.linspace(-1, 1, 9), method="synthetic FEM")
        fvm = grid_run(self.root / "requested", [-1, -.5, 0, .5, 1], [-1, 0, 1])
        for case in ({}, {"mesh": {"nx": 4}}, {"mesh": {"nx": 2, "ny": 4}},
                     {"mesh": {"nx": 4., "ny": 2}}):
            (fvm / "case.json").write_text(json.dumps(case), encoding="utf-8")
            with self.subTest(case=case), self.assertRaises(InputInvalid):
                self.gate.same_point_error(fvm, fem)
        (fvm / "case.json").unlink()
        with self.assertRaises(InputInvalid):
            self.gate.same_point_error(fvm, fem)

    def test_same_point_accepts_case_beside_result_directory(self):
        fvm = grid_run(self.root / "input" / "result", [-1, -.5, 0, .5, 1], [-1, 0, 1])
        (fvm / "case.json").rename(fvm.parent / "case.json")
        fem = grid_run(self.root / "reference", np.linspace(-1, 1, 9),
                       np.linspace(-1, 1, 9), method="synthetic FEM")
        self.assertEqual(self.gate.same_point_error(fvm, fem)["n_samples"], 8)

    def test_rigid_motion_does_not_change_known_deformation_maximum(self):
        run = grid_run(self.root / "deform", [-1, 0, 1], [-1, 0, 1])
        with h5py.File(run / "nested/first.vtkhdf", "r+") as data:
            points = data["VTKHDF/hex/Points"][:]
            q = points[:, 0]**2 - 2 / 3
            deformation = np.column_stack((q, np.zeros_like(q), np.zeros_like(q)))
            # Cartesian symmetry makes this field orthogonal to all six rigid modes.
            for translation, rotation in (([0, 0, 0], [0, 0, 0]),
                                           ([2, -3, 4], [.3, -.7, .2])):
                data["VTKHDF/hex/PointData/U"][:] = (
                    deformation + translation + np.cross(rotation, points - [.7, -.2, 1.3]))
                data.flush()
                actual = self.gate.deformation_max(run)
                self.assertTrue(math.isclose(actual, 2 / 3, rel_tol=1e-10))

    def test_deformation_rejects_absent_or_nonfinite_displacement(self):
        for kind in ("absent", "nan", "inf"):
            run = grid_run(self.root / kind, [-1, 0, 1], [-1, 0, 1])
            with h5py.File(run / "nested/first.vtkhdf", "r+") as data:
                if kind == "absent":
                    del data["VTKHDF/hex/PointData/U"]
                    del data["VTKHDF/cut/PointData/U"]
                else:
                    data["VTKHDF/hex/PointData/U"][0] = [float(kind), 0, 0]
            with self.subTest(kind=kind), self.assertRaises(InputInvalid):
                self.gate.deformation_max(run)


class TopGeometryTests(unittest.TestCase):
    def setUp(self):
        self.gate = gate_module()
        self.temp = tempfile.TemporaryDirectory(prefix="warpage-top-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def test_hex_uses_four_top_vertices_in_cell_order_and_first_step_file(self):
        run = grid_run(self.root / "hex", [0, 1, 3, 6], [0, 2, 6])
        expected = [[.5, 1], [2, 1], [4.5, 1], [.5, 4], [2, 4], [4.5, 4]]
        np.testing.assert_allclose(self.gate.top_face_centroids(run, (3, 2)), expected,
                                   rtol=0, atol=1e-14)

    def test_finest_hierarchy_dimensions_match_requested_case(self):
        run = grid_run(self.root / "hierarchy", np.linspace(0, 1, 49), np.linspace(0, 1, 17))
        change_manifest(run, lambda m: m["mesh"].update(nx0=24, ny0=8, L=1))
        self.assertEqual(self.gate.top_face_centroids(run, (48, 16)).shape, (768, 2))
        for changes, request in (({"nx0": 48, "ny0": 16}, (48, 16)),
                                  ({"nx0": 24, "ny0": 8, "L": 0}, (48, 16)),
                                  ({"L": 1}, (16, 48)), ({"padded_cells": 1}, (48, 16))):
            change_manifest(run, lambda m: m["mesh"].update(
                nx0=24, ny0=8, L=1, padded_cells=0))
            change_manifest(run, lambda m: m["mesh"].update(changes))
            with self.subTest(changes=changes, request=request), self.assertRaises(InputInvalid):
                self.gate.top_face_centroids(run, request)

    def test_hierarchy_fields_and_requested_dimensions_are_required_valid_integers(self):
        run = grid_run(self.root / "invalid-metadata", [0, 1, 2, 3], [0, 1])
        for key in ("nx0", "ny0", "L", "padded_cells"):
            for value in (None, -1, .5, True, "1"):
                change_manifest(run, lambda m: m["mesh"].update(nx0=3, ny0=1, L=0, padded_cells=0))
                change_manifest(run, lambda m: m["mesh"].update({key: value}))
                with self.subTest(key=key, value=value), self.assertRaises(InputInvalid):
                    self.gate.top_face_centroids(run, (3, 1))
            change_manifest(run, lambda m: m["mesh"].update(nx0=3, ny0=1, L=0, padded_cells=0))
            change_manifest(run, lambda m: m["mesh"].pop(key))
            with self.subTest(missing=key), self.assertRaises(InputInvalid):
                self.gate.top_face_centroids(run, (3, 1))
        change_manifest(run, lambda m: m["mesh"].update(nx0=3, ny0=1, L=0, padded_cells=0))
        for request in (None, (3,), (3, 1, 1), (3., 1), (True, 1), (0, 1), [3, 1]):
            with self.subTest(request=request), self.assertRaises(InputInvalid):
                self.gate.top_face_centroids(run, request)

    def test_top_plane_uses_strict_absolute_tolerance(self):
        run = grid_run(self.root / "strict-top", [0, 1, 2, 3, 4], [0, 1])
        # The last top column remains at z=0; moving the preceding top columns
        # down by exactly 1e-14 must exclude them, rather than accept <=1e-14.
        with h5py.File(run / "nested/first.vtkhdf", "r+") as data:
            points = data["VTKHDF/hex/Points"][:]
            points[points[:, 2] == 0, 2] = -1
            mask = points[:, 2] == 1
            points[mask, 2] = 0
            points[mask & (points[:, 0] < 4), 2] = -1e-14
            data["VTKHDF/hex/Points"][:] = points
        with self.assertRaises(InputInvalid):
            self.gate.top_face_centroids(run, (4, 1))

    def test_cut_area_centroid_winding_disconnected_patches_and_global_top(self):
        for reverse in (False, True):
            run = cut_run(self.root / str(reverse), reverse=reverse)
            expected = [[7 / 9 + shift, 4 / 9] for shift in (0, 4, 9)]
            np.testing.assert_allclose(self.gate.top_face_centroids(run, (1, 1)), expected,
                                       rtol=1e-14, atol=1e-14)

    def test_uncut_sample_count_and_minimum_three_are_required(self):
        run = grid_run(self.root / "mismatch", [0, 1, 2, 3], [0, 1])
        change_manifest(run, lambda m: m["mesh"].update(nx0=4))
        with self.assertRaises(InputInvalid):
            self.gate.top_face_centroids(run, (4, 1))
        run = grid_run(self.root / "few", [0, 1, 2], [0, 1])
        with self.assertRaises(InputInvalid):
            self.gate.top_face_centroids(run, (2, 1))

    def test_multipart_and_malformed_hex_topology_are_rejected(self):
        for name in ("partition", "cutpartition", "offset", "connectivity", "coordinate", "shortcell"):
            run = grid_run(self.root / name, [0, 1, 2, 3], [0, 1])
            with h5py.File(run / "nested/first.vtkhdf", "r+") as data:
                group = data["VTKHDF/hex"]
                if name in ("partition", "cutpartition"):
                    if name == "cutpartition":
                        group = data["VTKHDF/cut"]
                    old = group["NumberOfPoints"][0]
                    del group["NumberOfPoints"]
                    group["NumberOfPoints"] = [old, 0]
                elif name == "offset":
                    group["Offsets"][1] = 999
                elif name == "connectivity":
                    group["Connectivity"][0] = -1
                elif name == "shortcell":
                    group["Offsets"][1] = 7
                else:
                    group["Points"][0] = [math.nan, 0, 0]
            with self.subTest(name=name), self.assertRaises(InputInvalid):
                self.gate.top_face_centroids(run, (3, 1))

    def test_cut_zero_area_face_and_invalid_face_offsets_are_rejected(self):
        for name in ("zeroarea", "offset", "connectivity"):
            run = cut_run(self.root / name)
            with h5py.File(run / "nested/first.vtkhdf", "r+") as data:
                group = data["VTKHDF/cut"]
                if name == "zeroarea":
                    group["Points"][4:8] = [[0, 0, 2], [1, 0, 2], [2, 0, 2], [3, 0, 2]]
                elif name == "offset":
                    group["FaceOffsets"][2] = 9999
                else:
                    group["FaceConnectivity"][4] = -1
            with self.subTest(name=name), self.assertRaises(InputInvalid):
                self.gate.top_face_centroids(run, (1, 1))


class ResourceTests(unittest.TestCase):
    def test_linux_memavailable_uses_kib(self):
        with patch.object(support.sys, "platform", "linux"), \
                patch.object(Path, "read_text", return_value="MemTotal: 99999999 kB\nMemAvailable: 4194304 kB\n"):
            self.assertEqual(support.available_memory_bytes(), 4 * support.GIB)

    def test_darwin_memory_uses_free_inactive_speculative_and_sysctl_page_size(self):
        vm_stat = ("Mach Virtual Memory Statistics: (page size of 4096 bytes)\n"
                   "Pages free: 100000.\nPages inactive: 200000.\n"
                   "Pages speculative: 224288.\nPages active: 999999.\n")
        with patch.object(support.sys, "platform", "darwin"), \
                patch.object(support.subprocess, "check_output", side_effect=[vm_stat, "8192\n"]):
            self.assertEqual(support.available_memory_bytes(), 4 * support.GIB)

    def test_four_gib_preflight_is_inclusive_and_insufficient_memory_is_failure(self):
        for available, passes in ((4 * support.GIB - 1, False), (4 * support.GIB, True)):
            with patch.object(support, "available_memory_bytes", return_value=available):
                if passes:
                    self.assertEqual(support.memory_preflight(), available)
                else:
                    with self.assertRaisesRegex(AssertionError, "资源不足，不跑"):
                        support.memory_preflight()

    def test_peak_rss_units_and_ordinary_limit(self):
        for platform, raw in (("linux", 1024), ("darwin", 1048576)):
            with patch.object(support.sys, "platform", platform):
                self.assertTrue(math.isclose(support.rss_mib(raw), 1, rel_tol=1e-15))
        for platform, raw in (("linux", 3072 * 1024), ("darwin", 3072 * 1024**2)):
            with patch.object(support.sys, "platform", platform):
                self.assertTrue(math.isclose(support.rss_mib(raw), 3072, rel_tol=1e-15))
                support.require_ordinary_resources(dict(rss_mib=support.rss_mib(raw), wall_s=600),
                                                   reference=True)
                with self.assertRaisesRegex(AssertionError, "资源超限"):
                    support.require_ordinary_resources(dict(rss_mib=support.rss_mib(raw + 1), wall_s=1))
        with self.assertRaisesRegex(AssertionError, "参考未收敛"):
            support.require_ordinary_resources(dict(rss_mib=1, wall_s=600.01), reference=True)
        support.require_ordinary_resources(dict(rss_mib=1, wall_s=600.01))

    def test_unsupported_platform_fails(self):
        with patch.object(support.sys, "platform", "win32"):
            with self.assertRaisesRegex(AssertionError, "平台不支持"):
                support.available_memory_bytes()
            with self.assertRaisesRegex(AssertionError, "平台不支持"):
                support.rss_mib(1)


class FixtureTests(unittest.TestCase):
    def test_prescribed_hex_and_cut_fixtures_load_without_the_new_gate_module(self):
        with tempfile.TemporaryDirectory(prefix="warpage-fixture-") as temp:
            root = Path(temp)
            hex_run = grid_run(root / "hex", [-1, 0, 1], [-1, 0, 1])
            self.assertEqual(load_run(hex_run).points.shape, (18, 3))
            self.assertEqual(len(load_run(cut_run(root / "cut")).cell_gid), 4)


if __name__ == "__main__":
    unittest.main(verbosity=2)
