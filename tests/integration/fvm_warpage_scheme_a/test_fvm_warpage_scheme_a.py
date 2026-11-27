#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
"""INT-a acceptance: analytic data only; absent implementation is an explicit failure."""
import importlib
import json
import math
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

try:
    import h5py
    import numpy as np
except ImportError as exc:
    print(f"Required acceptance dependency unavailable: {exc}", file=sys.stderr)
    sys.exit(77)

# Support direct execution from CTest and unittest discovery from the repository root.
HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(HERE))
from synthetic_inputs import (FVM_KEYS, METHOD, clone, cut_run, edit_json, gate_records,
                              hex_run, record, replace, tiers, write_manifest)

MODULE = "tools.vv.fvm_warpage_scheme_a"


class Acceptance(unittest.TestCase):
    def setUp(self):
        # Do not import at collection time: every acceptance case must run and fail red.
        try:
            self.api = importlib.import_module(MODULE)
        except ModuleNotFoundError as exc:
            if exc.name != MODULE:
                raise
            self.api = None
        if self.api is None:
            self.fail("Missing INT-a implementation: tools/vv/fvm_warpage_scheme_a.py")
        self.temp = tempfile.TemporaryDirectory(prefix="scheme-a-")
        self.addCleanup(self.temp.cleanup)
        self.work = Path(self.temp.name)

    def relative(self, actual, expected, tolerance=1e-12):
        self.assertTrue(math.isfinite(actual), repr(actual))
        self.assertLessEqual(abs((actual - expected) / expected), tolerance,
                             f"actual={actual:.17g}, expected={expected:.17g}")

    def invalid(self, function, *args):
        # The plan chooses one local ValueError subclass for invalid inputs.
        self.assertTrue(issubclass(self.api.InputInvalid, ValueError))
        with self.assertRaises(self.api.InputInvalid):
            function(*args)

    def verdict(self, result, expected):
        self.assertIs(result["pass"], expected, repr(result))
        self.assertIsInstance(result["checks"], list)
        if not expected:
            self.assertTrue(any(not c["pass"] and c["reason"] for c in result["checks"]),
                            repr(result))

    def reason(self, result, text):
        self.verdict(result, False)
        reasons = [c.get("reason", "") for c in result["checks"] if not c["pass"]]
        self.assertTrue(any(text in r for r in reasons), repr(result))

    def test_plane_plus_quadratic_analytic_pv(self):
        xy = np.array([(x, y) for x in (-1., -.5, 0., .5, 1.) for y in (-1., 0., 1.)])
        x, y = xy.T
        for kappa in (.01, -.01):
            w = .02 + .003 * x - .004 * y + kappa * x**2 / 2
            self.relative(self.api.plane_pv(xy, w), abs(kappa) * 2**2 / 8)

    def test_pure_plane_has_no_warpage(self):
        xy = np.array([(x, y) for x in (-1., 0., 1.) for y in (-1., 0., 1.)])
        self.assertLessEqual(self.api.plane_pv(xy, .02 + .003 * xy[:, 0] - .004 * xy[:, 1]),
                             1e-15)

    def test_plane_pv_reuses_existing_plane_removal(self):
        from tools.vv import warpage_qoi
        xy = np.array([(x, y) for x in (-1., 0., 1.) for y in (-1., 0., 1.)])
        with mock.patch.object(self.api, "best_fit_plane", wraps=warpage_qoi.best_fit_plane) as fit:
            self.relative(self.api.plane_pv(xy, .005 * xy[:, 0]**2), .005)
        self.assertEqual(fit.call_count, 1)

    def sampling_ratio(self, nx):
        nodes = np.array([(x, y) for x in np.arange(nx + 1) / nx for y in (-.5, .5)])
        centers = np.array([(x, y) for x in (np.arange(nx) + .5) / nx for y in (-.5, .5)])
        node_pv = self.api.plane_pv(nodes, .005 * nodes[:, 0]**2)
        face_pv = self.api.plane_pv(centers, .005 * centers[:, 0]**2)
        expected = 1 - 2 / nx if nx % 2 == 0 else (nx - 1) / (nx + 1)
        self.relative(face_pv / node_pv, expected)

    def grid(self, nx0, ny0, level):
        run, _, expected = hex_run(self.work / "run", nx0, ny0, level)
        xy = self.api.top_face_centroids(run, (nx0 * 2**level, ny0 * 2**level))
        self.assertEqual(xy.shape, expected.shape)
        self.assertEqual(xy.dtype, np.dtype("float64"))
        # The brief explicitly requires bitwise equality on this dyadic grid.
        np.testing.assert_array_equal(xy.view(np.uint64), expected.view(np.uint64))

    def test_refined_mesh_cannot_be_inferred_from_requested_size(self):
        run, _, _ = hex_run(self.work / "run", 48, 16, 1)
        self.invalid(self.api.top_face_centroids, run, (48, 16))

    def test_unrefined_mesh_cannot_satisfy_refined_request(self):
        run, _, _ = hex_run(self.work / "run", 24, 8, 0)
        self.invalid(self.api.top_face_centroids, run, (48, 16))

    def mesh_invalid(self, field, value, missing=False):
        run, _, _ = hex_run(self.work / "run")
        def change(m):
            if missing:
                del m["mesh"][field]
            else:
                m["mesh"][field] = value
        edit_json(run / "manifest.json", change)
        self.invalid(self.api.top_face_centroids, run, (8, 4))

    def topology_invalid(self, block, field, value):
        polygons = [[(0., 0.), (4., 0.), (2., 2.), (0., 2.)],
                    [(8., 0.), (12., 0.), (10., 2.), (8., 2.)],
                    [(16., 0.), (20., 0.), (18., 2.), (16., 2.)]]
        if block == "hex":
            run, step, _ = hex_run(self.work / "run")
        else:
            run, step = cut_run(self.work / "run", polygons)
        with h5py.File(step, "r+") as data:
            b = data["VTKHDF/" + block]
            if value is None:
                del b[field]
            elif callable(value):
                values = b[field][:]
                value(values)
                replace(b, field, values)
            else:
                replace(b, field, value)
        self.invalid(self.api.top_face_centroids, run, (8, 4) if block == "hex" else (3, 1))

    def test_cut_area_centroids_and_disconnected_faces(self):
        # Trapezoid area=6; first moments Mx=28/3, My=16/3 => (14/9,8/9).
        # Vertex average (3/2,1) would be wrong. Translations preserve the moments.
        polygons = [[(dx, 0.), (dx + 4., 0.), (dx + 2., 2.), (dx, 2.)]
                    for dx in (0., 8., 16.)]
        run, _ = cut_run(self.work / "run", polygons)
        expected = np.array([(dx + 14 / 9, 8 / 9) for dx in (0., 8., 16.)])
        np.testing.assert_allclose(self.api.top_face_centroids(run, (3, 1)), expected,
                                   rtol=1e-14, atol=0)

    def test_reversed_cut_winding_preserves_area_centroids(self):
        polygons = [[(dx, 0.), (dx + 4., 0.), (dx + 2., 2.), (dx, 2.)]
                    for dx in (0., 8., 16.)]
        run, _ = cut_run(self.work / "run", polygons, reverse=True)
        np.testing.assert_allclose(self.api.top_face_centroids(run, (3, 1)),
                                   [(dx + 14 / 9, 8 / 9) for dx in (0., 8., 16.)],
                                   rtol=1e-14, atol=0)

    def test_zero_area_top_polygon_is_invalid(self):
        run, _ = cut_run(self.work / "run", [[(dx, 0.), (dx + 1., 0.), (dx + 2., 0.)]
                                             for dx in (0., 4., 8.)])
        self.invalid(self.api.top_face_centroids, run, (3, 1))

    def test_cut_sample_count_is_not_uncut_rectangle_count(self):
        run, _ = cut_run(self.work / "run", [[(dx, 0.), (dx + 1., 0.), (dx, 1.)]
                                             for dx in (0., 4., 8., 12.)])
        self.assertEqual(self.api.top_face_centroids(run, (3, 1)).shape, (4, 2))

    def test_global_top_and_hex_then_cut_order(self):
        polygons = [[(dx, 0.), (dx + 1., 0.), (dx, 1.)] for dx in (4., 8., 12.)]
        run, step = cut_run(self.work / "run", polygons)
        _, hex_step, expected_hex = hex_run(self.work / "hex", 3, 1)
        with h5py.File(step, "r+") as data, h5py.File(hex_step) as source:
            del data["VTKHDF/hex"]
            source.copy("VTKHDF/hex", data["VTKHDF"], name="hex")
        expected_cut = np.array([(dx + 1 / 3, 1 / 3) for dx in (4., 8., 12.)])
        np.testing.assert_allclose(self.api.top_face_centroids(run, (3, 1)),
                                   np.vstack([expected_hex, expected_cut]), rtol=1e-14, atol=0)
        with h5py.File(step, "r+") as data:
            p = data["VTKHDF/hex/Points"][:]
            p[:, 2] -= 2
            replace(data["VTKHDF/hex"], "Points", p)
        np.testing.assert_allclose(self.api.top_face_centroids(run, (3, 1)), expected_cut,
                                   rtol=1e-14, atol=0)

    def z_threshold(self, delta, passes):
        run, step, expected = hex_run(self.work / "run")
        with h5py.File(step, "r+") as data:
            p = data["VTKHDF/hex/Points"][:]
            p[:, 2] -= 1  # global top=0: the strict 1e-14 boundary is representable.
            p[4, 2] = -delta
            replace(data["VTKHDF/hex"], "Points", p)
        if passes:
            np.testing.assert_array_equal(self.api.top_face_centroids(run, (8, 4)), expected)
        else:
            self.invalid(self.api.top_face_centroids, run, (8, 4))

    def test_insufficient_hex_samples(self):
        run, _, _ = hex_run(self.work / "run", 2, 1)
        self.invalid(self.api.top_face_centroids, run, (2, 1))

    def test_insufficient_cut_samples(self):
        run, _ = cut_run(self.work / "run", [[(0., 0.), (1., 0.), (0., 1.)]])
        self.invalid(self.api.top_face_centroids, run, (3, 1))

    def requested_invalid(self, requested):
        run, _, _ = hex_run(self.work / "run")
        self.invalid(self.api.top_face_centroids, run, requested)

    def manifest_invalid(self, change):
        run, _, _ = hex_run(self.work / "run")
        if change == "missing":
            (run / "manifest.json").unlink()
        elif change == "malformed":
            (run / "manifest.json").write_text("{", encoding="utf-8")
        elif change == "missing_hdf":
            (run / "out/first.vtkhdf").unlink()
        else:
            edit_json(run / "manifest.json", change)
        self.invalid(self.api.top_face_centroids, run, (8, 4))

    def array_invalid(self, function, first, second):
        self.invalid(getattr(self.api, function), first, second)

    def test_empty_mesh_is_invalid(self):
        run, step, _ = hex_run(self.work / "run")
        with h5py.File(step, "r+") as data:
            b = data["VTKHDF/hex"]
            for name in ("Points", "Connectivity", "Types"):
                replace(b, name, np.empty((0, 3)) if name == "Points" else np.array([], dtype=int))
            for name in ("NumberOfPoints", "NumberOfCells", "NumberOfConnectivityIds", "Offsets"):
                replace(b, name, np.array([0], dtype=int))
        self.invalid(self.api.top_face_centroids, run, (8, 4))

    def same_inputs(self):
        fvm, _, expected = hex_run(self.work / "fvm", hx=.25, hy=.5)
        fem = write_manifest(self.work / "fem", dict(nx0=12, ny0=4, L=0, padded_cells=0),
                             pv=.008, method="FEM top nodes; least-squares plane removal")
        edit_json(fvm / "manifest.json", lambda m: m["summary"][0].update(value=.006))
        return fvm, fem, expected

    def analytic_callback(self, expected, fem, calls):
        def evaluate(run_dir, xy):
            self.assertEqual(Path(run_dir), fem)
            np.testing.assert_array_equal(xy, expected)
            calls.append(xy.copy())
            x, y = xy.T
            # Analytic tangent subtraction: the symmetric quadratic's fitted
            # plane differs only by a constant, which cannot change PV.
            w = .01 * (x*x + y*y) / 2 + .002 + .003*x - .004*y
            plane = .002 + .003*x - .004*y + .01 * (x + y - 1)
            return float(np.ptp(w - plane))
        return evaluate

    def test_same_point_analytic_callback_signed_errors_and_keys(self):
        fvm, fem, expected = self.same_inputs()
        calls = []
        result = self.api.same_point_record(fvm, fem, self.analytic_callback(expected, fem, calls))
        self.assertEqual(len(calls), 1)
        self.assertTrue({"fvm_pv", "fem_same_point_pv", "same_point_rel", "fem_nodal_pv",
                         "nodal_rel", "n_samples"}.issubset(result))
        self.assertFalse(any(k == "interp_indicator" or k.startswith("interp_indicator_")
                             for k in result))
        # .005 * ((.875^2-.125^2) + (.75^2-.25^2)) = .00625.
        self.relative(result["fem_same_point_pv"], .00625)
        self.relative(result["fvm_pv"], .006)
        self.relative(result["fem_nodal_pv"], .008)
        self.relative(result["same_point_rel"], -.04)
        self.relative(result["nodal_rel"], -.25)
        self.assertEqual(result["n_samples"], 32)
        self.assertEqual(float(result["same_point_rel"]).hex(),
                         ((result["fvm_pv"] - result["fem_same_point_pv"]) /
                          result["fem_same_point_pv"]).hex())
        self.assertEqual(float(result["nodal_rel"]).hex(),
                         (result["fvm_pv"] / result["fem_nodal_pv"] - 1).hex())

    def test_same_point_positive_error_preserves_sign(self):
        fvm, fem, expected = self.same_inputs()
        edit_json(fvm / "manifest.json", lambda m: m["summary"][0].update(value=.0065))
        result = self.api.same_point_record(fvm, fem, self.analytic_callback(expected, fem, []))
        self.relative(result["same_point_rel"], .04)

    def test_same_point_selects_first_step_summary(self):
        fvm, fem, expected = self.same_inputs()
        for run in (fvm, fem):
            def change(m):
                m["steps"].append({"n": 8, "file": "does-not-exist.vtkhdf"})
                entry = dict(m["summary"][0], step=8, value=99)
                m["summary"].insert(0, entry)
            edit_json(run / "manifest.json", change)
        result = self.api.same_point_record(fvm, fem, self.analytic_callback(expected, fem, []))
        self.relative(result["fvm_pv"], .006)
        self.relative(result["fem_nodal_pv"], .008)

    def case_invalid(self, change):
        fvm, fem, _ = self.same_inputs()
        if change is None:
            (fvm / "case.json").unlink()
        else:
            edit_json(fvm / "case.json", change)
        self.invalid(self.api.same_point_record, fvm, fem, lambda *_: .00625)

    def callback_invalid(self, callback):
        fvm, fem, _ = self.same_inputs()
        self.invalid(self.api.same_point_record, fvm, fem, callback)

    def summary_invalid(self, side, change):
        fvm, fem, _ = self.same_inputs()
        edit_json((fvm if side == "fvm" else fem) / "manifest.json", change)
        self.invalid(self.api.same_point_record, fvm, fem, lambda *_: .00625)

    def convergence_boundary(self, quantity, d, passes):
        data = tiers()
        coarse = 1.0 * (1 - d)
        if quantity == "thickness":
            data[0]["same_point_pv"][FVM_KEYS[0]] = coarse
            # Isolate thickness: all kz2 tiers agree, while kz4 remains 1.
            for tier in data[2:]:
                tier["same_point_pv"][FVM_KEYS[0]] = coarse
        elif quantity == "same":
            data[0]["same_point_pv"][FVM_KEYS[0]] = coarse
            data[1]["same_point_pv"][FVM_KEYS[0]] = coarse
            if not passes:
                # No later adjacent pair can rescue the intentionally failing sequence.
                for i, tier in enumerate(data[2:]):
                    tier["same_point_pv"][FVM_KEYS[0]] = (1 - d)**(-i)
        else:
            data[0]["nodal_pv"] = coarse
            if not passes:
                for i, tier in enumerate(data[2:]):
                    tier["nodal_pv"] = (1 - d)**(-i)
        result = self.api.fem_reference_self_convergence(data)
        self.verdict(result, passes)
        if not passes:
            self.reason(result, "FEM kz2 厚度向未收敛" if quantity == "thickness" else "参考未收敛")
        else:
            self.assertEqual(result["reference"][FVM_KEYS[0]], (24, 8, 2))

    def test_reference_selected_independently_for_each_fvm_key(self):
        data = tiers()
        data[0]["same_point_pv"][FVM_KEYS[1]] = .99
        data[1]["same_point_pv"][FVM_KEYS[1]] = .99
        data[0]["same_point_pv"][FVM_KEYS[2]] = .98
        data[1]["same_point_pv"][FVM_KEYS[2]] = .98
        data[2]["same_point_pv"][FVM_KEYS[2]] = .99
        result = self.api.fem_reference_self_convergence(data)
        self.verdict(result, True)
        self.assertEqual(result["reference"], {FVM_KEYS[0]: (24, 8, 2),
                                               FVM_KEYS[1]: (48, 16, 2),
                                               FVM_KEYS[2]: (96, 32, 2)})

    def test_reference_node_only_failure_at_upper_limit(self):
        data = tiers()
        for tier, nodal in zip([data[0], *data[2:]], (1., 1.01, 1.02, 1.03)):
            tier["nodal_pv"] = nodal
        self.reason(self.api.fem_reference_self_convergence(data), "参考未收敛")

    def test_reference_thickness_checked_for_each_fvm_key(self):
        data = tiers()
        data[0]["same_point_pv"][FVM_KEYS[-1]] = .99
        self.reason(self.api.fem_reference_self_convergence(data), "FEM kz2 厚度向未收敛")

    def test_reference_selection_independent_of_tier_input_order(self):
        data = tiers()
        self.assertEqual(self.api.fem_reference_self_convergence(list(reversed(data))),
                         self.api.fem_reference_self_convergence(data))

    def reference_invalid(self, change):
        data = tiers()
        change(data)
        self.verdict(self.api.fem_reference_self_convergence(data), False)

    def test_convergence_and_gate_validate_quantity_before_calculation(self):
        class QuantityGuard(ValueError):
            pass
        for function, data in ((self.api.fem_reference_self_convergence, tiers()),
                               (self.api.scheme_a_gate, gate_records())):
            with mock.patch.object(self.api, "require_convergence_quantity",
                                   side_effect=QuantityGuard("quantity guard")) as guard:
                with self.assertRaises(QuantityGuard):
                    function(data)
                guard.assert_called_once_with("warpage_pv")

    def test_scheme_a_complete_pass_and_check_schema(self):
        result = self.api.scheme_a_gate(gate_records())
        self.verdict(result, True)
        self.assertTrue(result["checks"])
        for check in result["checks"]:
            self.assertTrue({"name", "value", "limit", "pass", "reason"}.issubset(check))
            self.assertIsInstance(check["name"], str)
            self.assertIs(check["pass"], True)

    def gate_absolute(self, nx, kz, error, passes, limit):
        data = gate_records()
        # At the kz8 absolute boundary preserve p_kz>=1.5 and p_xy>=1.
        if kz == 8 and nx == 96:
            record(data, 48, 4)["same_point_rel"] = .03
            record(data, 48, 8)["same_point_rel"] = 2 * error
            record(data, 96, 4)["same_point_rel"] = .015
        elif kz == 8:
            record(data, 48, 4)["same_point_rel"] = .03
        elif nx == 96:
            record(data, 48, 4)["same_point_rel"] = .03
        record(data, nx, kz)["same_point_rel"] = error
        result = self.api.scheme_a_gate(data)
        self.verdict(result, passes)
        if not passes:
            self.assertTrue(any(not c["pass"] and c["limit"] == limit
                                for c in result["checks"]), repr(result))

    def gate_order(self, axis, target, passes):
        data = gate_records()
        fine = 2.0**-9
        coarse = math.exp2(target) * fine
        # Required bitwise precondition: no tolerance masks an incorrectly constructed boundary.
        self.assertEqual(math.log2(coarse / fine).hex(), target.hex())
        if axis == "kz":
            record(data, 96, 4)["same_point_rel"] = coarse
            record(data, 96, 8)["same_point_rel"] = fine
        else:
            record(data, 48, 4)["same_point_rel"] = 4 * coarse
            record(data, 96, 4)["same_point_rel"] = 4 * fine
            record(data, 48, 8)["same_point_rel"] = coarse
            record(data, 96, 8)["same_point_rel"] = fine
        # Assert the pair actually handed to the gate, as well as its construction.
        actual_coarse = record(data, 96 if axis == "kz" else 48,
                               4 if axis == "kz" else 8)["same_point_rel"]
        actual_fine = record(data, 96, 8)["same_point_rel"]
        self.assertEqual(math.log2(actual_coarse / actual_fine).hex(), target.hex())
        self.verdict(self.api.scheme_a_gate(data), passes)

    def test_gate_missing_kz8_has_required_reason(self):
        data = [r for r in gate_records() if r["kz"] == 4]
        self.reason(self.api.scheme_a_gate(data), "缺 kz≥8 运行")

    def test_gate_partial_kz_pair_is_failure(self):
        data = [r for r in gate_records() if not (r["nx"] == 96 and r["kz"] == 8)]
        self.verdict(self.api.scheme_a_gate(data), False)

    def test_gate_missing_kz4_cannot_bypass_original_gate(self):
        self.verdict(self.api.scheme_a_gate([r for r in gate_records() if r["kz"] == 8]), False)

    def gate_indeterminate(self, coarse, fine):
        data = gate_records()
        record(data, 96, 4)["same_point_rel"] = coarse
        record(data, 96, 8)["same_point_rel"] = fine
        self.reason(self.api.scheme_a_gate(data), "阶不可判")

    def gate_field(self, field, value, passes=False):
        data = gate_records()
        record(data, 96, 8)[field] = value
        self.verdict(self.api.scheme_a_gate(data), passes)

    def test_gate_negative_same_sign_errors_pass(self):
        data = gate_records()
        for r in data:
            r["same_point_rel"] *= -1
        self.verdict(self.api.scheme_a_gate(data), True)

    def test_gate_kz16_diagnostics_do_not_change_verdict(self):
        data = gate_records()
        baseline = self.api.scheme_a_gate(data)
        data.append(dict(nx=96, ny=32, kz=16, same_point_rel=float("nan"),
                         deformation_rel=99., fvm_curvature=0., fem_curvature=1.))
        self.assertEqual(self.api.scheme_a_gate(data), baseline)

    def test_gate_ignores_interpolation_fields(self):
        for error in (.003, .0050001):
            data = gate_records()
            record(data, 96, 8)["same_point_rel"] = error
            baseline = self.api.scheme_a_gate(data)
            self.verdict(baseline, error == .003)
            for r in data:
                r.update(interp_indicator=float("inf"), interp_indicator_legacy="invalid")
            self.assertEqual(self.api.scheme_a_gate(data), baseline)

    def test_gate_checks_are_deterministic_when_records_reordered(self):
        data = gate_records()
        self.assertEqual(self.api.scheme_a_gate(list(reversed(data))), self.api.scheme_a_gate(data))

    def test_gate_duplicate_record_is_failure(self):
        data = gate_records()
        data.append(clone(data[0]))
        self.verdict(self.api.scheme_a_gate(data), False)

    def gate_missing(self, field):
        data = gate_records()
        del data[3][field]
        self.verdict(self.api.scheme_a_gate(data), False)

    def test_gate_empty_input_fails(self):
        self.verdict(self.api.scheme_a_gate([]), False)

    def gate_192(self, field, value, passes, kz=8):
        data = gate_records()
        data.extend([dict(data[2], nx=192, ny=64, same_point_rel=.006),
                     dict(data[3], nx=192, ny=64, same_point_rel=.0015)])
        record(data, 192, kz)[field] = value
        self.verdict(self.api.scheme_a_gate(data), passes)

    def precondition(self, kz, same, node, limit, passes):
        result = self.api.scheme_a_preconditions(96, kz, same, node, limit)
        self.verdict(result, passes)
        if not passes:
            self.assertTrue(result["reason"])
        if node is None:
            self.assertEqual(len(result["checks"]), 2)

    def quantity(self, name, allowed):
        if allowed:
            self.api.require_convergence_quantity(name)
        else:
            with self.assertRaises(ValueError):
                self.api.require_convergence_quantity(name)

    def test_deformation_removes_translation_and_infinitesimal_rotation(self):
        from tools.vv import field_compare
        points = np.array([(x, y, z) for x in (-1., 1.) for y in (-2., 2.) for z in (-.5, .5)])
        # Uniform expansion is zero-mean and orthogonal to each rigid rotation.
        deformation = .02 * points
        displacement = deformation + np.array([.3, -.2, .1]) + np.cross([.03, -.02, .04], points)
        with mock.patch.object(self.api, "_remove_rigid", wraps=field_compare._remove_rigid) as rigid:
            maximum = self.api.deformation_max(points, displacement)
        self.relative(maximum, .02 * math.sqrt(1 + 4 + .25), tolerance=1e-10)
        self.assertEqual(rigid.call_count, 1)

    def test_rigid_motion_alone_has_zero_deformation(self):
        p = np.array([(x, y, z) for x in (-1., 1.) for y in (-1., 1.) for z in (-1., 1.)])
        self.assertLessEqual(self.api.deformation_max(p, [.1, .2, -.3] + np.cross([.01, .02, .03], p)),
                             1e-14)

    def test_run_measured_has_per_child_not_cumulative_rss(self):
        # A fork/exec child may retain the test parent's pre-exec RSS floor.
        # Isolate from earlier large geometry allocations, keeping real wait4
        # measurements and both original 60 MiB assertions unchanged.
        probe = """
import json, os, sys
from tools.vv.fvm_warpage_scheme_a import run_measured
high = run_measured([sys.executable, '-c',
                    "x=bytearray(64*1024*1024); x[::4096]=b'x'*16384"],
                   dict(os.environ), sys.argv[1])
low = run_measured([sys.executable, '-c', 'pass'], dict(os.environ), sys.argv[1])
print(json.dumps([high, low]))
"""
        high, low = json.loads(subprocess.check_output(
            [sys.executable, "-c", probe, str(self.work)], cwd=ROOT, text=True))
        print(f"per-child resource probe: high={high}, low={low}", flush=True)
        for result in (high, low):
            self.assertEqual(result["returncode"], 0)
            self.assertGreaterEqual(result["wall_s"], 0)
            self.assertTrue(math.isfinite(result["wall_s"]))
        self.assertGreaterEqual(high["peak_rss_mib"], 60)
        self.assertLess(low["peak_rss_mib"], 60)

    def test_run_measured_exit_status_env_cwd_and_wait4(self):
        env = dict(os.environ, CHIPCAE_SCHEME_A_CHILD="present")
        script = ("import os,sys; from pathlib import Path; "
                  "Path('child.txt').write_text(os.environ['CHIPCAE_SCHEME_A_CHILD']); sys.exit(7)")
        with mock.patch("os.wait4", wraps=os.wait4) as wait:
            result = self.api.run_measured([sys.executable, "-c", script], env, self.work)
        self.assertEqual(result["returncode"], 7)
        self.assertEqual((self.work / "child.txt").read_text(), "present")
        self.assertEqual(wait.call_count, 1)
        self.assertEqual(wait.call_args.args[1], 0)
        self.assertGreaterEqual(result["wall_s"], 0)

    def test_run_measured_decodes_signal_status(self):
        result = self.api.run_measured([sys.executable, "-c",
                                       "import os,signal; os.kill(os.getpid(), signal.SIGTERM)"],
                                      dict(os.environ), self.work)
        self.assertEqual(result["returncode"], -signal.SIGTERM)

    def test_run_measured_inherits_streams_and_sets_returncode_after_reap(self):
        children = []
        popen = subprocess.Popen
        def spawn(*args, **kwargs):
            self.assertNotEqual(kwargs.get("stdout"), subprocess.PIPE)
            self.assertNotEqual(kwargs.get("stderr"), subprocess.PIPE)
            child = popen(*args, **kwargs)
            children.append(child)
            return child
        with mock.patch("subprocess.Popen", side_effect=spawn):
            result = self.api.run_measured([sys.executable, "-c", "raise SystemExit(3)"],
                                          dict(os.environ), self.work)
        self.assertEqual(result["returncode"], 3)
        self.assertEqual(len(children), 1)
        self.assertEqual(children[0].returncode, 3)
        # wait4 already reaped this pid; Popen must not attempt a second reap.
        with self.assertRaises(ChildProcessError):
            os.wait4(children[0].pid, os.WNOHANG)

    def rss(self, value, platform, expected):
        if expected is None:
            self.invalid(self.api.rss_mib, value, platform)
        else:
            self.relative(self.api.rss_mib(value, platform), expected)

    def memory(self, platform, meminfo="", vmstat="", pagesize="4096", failure=None):
        # Mock only OS I/O, leaving the parser and arithmetic real. Support either
        # Path.read_text or open, and either platform.system or sys.platform.
        def read_text(path, *args, **kwargs):
            if Path(path).as_posix() != "/proc/meminfo":
                raise AssertionError(f"Unexpected memory input: {path}")
            if failure:
                raise failure
            return meminfo
        def output(argv, *args, **kwargs):
            command = list(argv) if not isinstance(argv, str) else argv.split()
            if failure:
                raise failure
            if command == ["vm_stat"]:
                result = vmstat
            elif command == ["sysctl", "-n", "hw.pagesize"]:
                result = pagesize
            else:
                raise AssertionError(f"Unexpected memory command: {command}")
            return result if kwargs.get("text") or kwargs.get("encoding") or kwargs.get("universal_newlines") else result.encode()
        def run(argv, *args, **kwargs):
            return subprocess.CompletedProcess(argv, 0, stdout=output(argv, *args, **kwargs),
                                               stderr="" if kwargs.get("text") else b"")
        opener = mock.mock_open(read_data=meminfo)
        if failure:
            opener.side_effect = failure
        with mock.patch("sys.platform", platform), \
                mock.patch("platform.system", return_value={"linux": "Linux", "darwin": "Darwin"}.get(platform, platform)), \
                mock.patch("builtins.open", opener), \
                mock.patch.object(Path, "read_text", read_text), \
                mock.patch("subprocess.check_output", side_effect=output), \
                mock.patch("subprocess.run", side_effect=run):
            return self.api.mem_available_bytes()

    def test_linux_memavailable_uses_available_not_free(self):
        self.assertEqual(self.memory("linux", meminfo="MemFree: 1 kB\nMemAvailable: 12345 kB\n"),
                         12345 * 1024)

    def test_darwin_available_sums_required_categories_and_sysctl_pagesize(self):
        # Header says 4096; the required sysctl result (16384) governs conversion.
        vm = ("Mach Virtual Memory Statistics: (page size of 4096 bytes)\n"
              "Pages free: 10.\nPages active: 999.\nPages inactive: 20.\n"
              "Pages speculative: 3.\nPages wired down: 888.\n")
        self.assertEqual(self.memory("darwin", vmstat=vm, pagesize="16384\n"), 33 * 16384)

    def memory_invalid(self, platform, meminfo="", vmstat="", pagesize="4096", failure=None):
        self.invalid(self.memory, platform, meminfo, vmstat, pagesize, failure)


# Generate separate unittest cases, so each parameter has its own red assertion
# even when the module is absent. Values are bound now, never via a late closure.
def case(name, method, *args):
    def test(self):
        getattr(self, method)(*clone(args))
    test.__name__ = "test_" + name
    setattr(Acceptance, test.__name__, test)


for nx in (24, 25, 48, 96, 192):
    case(f"sampling_ratio_nx_{nx}", "sampling_ratio", nx)
for nx, ny, level in ((24, 8, 0), (48, 16, 0), (24, 8, 1)):
    case(f"dyadic_centroids_{nx}_{ny}_L{level}", "grid", nx, ny, level)
for tag, requested in (("missing", None), ("short", (8,)), ("extra", (8, 4, 1)),
                        ("bool", (True, 4)), ("float", (8., 4)), ("zero", (0, 4))):
    case("requested_dimensions_" + tag, "requested_invalid", requested)
for tag, change in (
        ("missing", "missing"), ("malformed", "malformed"), ("hdf_missing", "missing_hdf"),
        ("empty_steps", lambda m: m.update(steps=[])),
        ("step_file_missing", lambda m: m["steps"][0].pop("file")),
        ("mesh_missing", lambda m: m.pop("mesh"))):
    case("manifest_" + tag, "manifest_invalid", change)
for function, dim in (("plane_pv", 2), ("deformation_max", 3)):
    for tag, first, second in (
            ("empty", np.empty((0, dim)), np.empty((0, 3)) if dim == 3 else np.empty(0)),
            ("shape", np.zeros((4, dim + 1)), np.zeros((4, 3)) if dim == 3 else np.zeros(4)),
            ("length", np.zeros((4, dim)), np.zeros((3, 3)) if dim == 3 else np.zeros(3)),
            ("nonfinite_geometry", np.full((4, dim), math.inf), np.zeros((4, 3)) if dim == 3 else np.zeros(4)),
            ("nonfinite_values", np.zeros((4, dim)), np.full((4, 3), math.nan) if dim == 3 else np.full(4, math.nan))):
        case(function + "_invalid_" + tag, "array_invalid", function, first, second)
for field in ("nx0", "ny0", "L", "padded_cells"):
    case(f"mesh_missing_{field}", "mesh_invalid", field, None, True)
    for tag, value in (("bool", True), ("float", 1.0), ("string", "1"), ("null", None)):
        case(f"mesh_{field}_{tag}", "mesh_invalid", field, value)
for field, value in (("nx0", 0), ("ny0", -1), ("L", -1), ("padded_cells", 1)):
    case(f"mesh_range_{field}", "mesh_invalid", field, value)
for tag, delta, passes in (("inside", np.nextafter(1e-14, 0), True),
                            ("boundary", 1e-14, False),
                            ("outside", np.nextafter(1e-14, math.inf), False)):
    case("strict_z_" + tag, "z_threshold", delta, passes)

for block in ("hex", "cut"):
    for field in ("NumberOfPoints", "NumberOfCells", "NumberOfConnectivityIds", "Points",
                  "Connectivity", "Offsets"):
        case(f"{block}_missing_{field}", "topology_invalid", block, field, None)
    for tag, field, value in (
            ("multipart", "NumberOfPoints", np.array([8, 8], dtype=np.int64)),
            ("empty_partition_count", "NumberOfPoints", np.array([], dtype=np.int64)),
            ("noninteger_count", "NumberOfPoints", np.array([1.5])),
            ("count_mismatch", "NumberOfPoints", np.array([1], dtype=np.int64)),
            ("cell_count", "NumberOfCells", np.array([9999], dtype=np.int64)),
            ("bad_offset", "Offsets", lambda a: a.__setitem__(-1, 1)),
            ("bad_connectivity", "Connectivity", lambda a: a.__setitem__(0, 999999)),
            ("negative_connectivity", "Connectivity", lambda a: a.__setitem__(0, -1)),
            ("nan_coordinates", "Points", lambda a: a.__setitem__((0, 0), math.nan))):
        case(f"{block}_{tag}", "topology_invalid", block, field, value)
for field in ("FaceConnectivity", "FaceOffsets", "PolyhedronToFaces", "PolyhedronOffsets"):
    case("cut_missing_" + field, "topology_invalid", "cut", field, None)
for tag, field, change in (
        ("face_offset", "FaceOffsets", lambda a: a.__setitem__(-1, 1)),
        ("face_point", "FaceConnectivity", lambda a: a.__setitem__(0, 999999)),
        ("face_reference", "PolyhedronToFaces", lambda a: a.__setitem__(0, 999999)),
        ("negative_face_reference", "PolyhedronToFaces", lambda a: a.__setitem__(0, -1)),
        ("polyhedron_offset", "PolyhedronOffsets", lambda a: a.__setitem__(-1, 1))):
    case("cut_invalid_" + tag, "topology_invalid", "cut", field, change)

case("missing_case_json", "case_invalid", None)
case("missing_case_mesh", "case_invalid", lambda c: c.pop("mesh"))
for field in ("nx", "ny"):
    case("missing_case_" + field, "case_invalid", lambda c, f=field: c["mesh"].pop(f))
    for tag, value in (("mismatch", 9), ("bool", True), ("string", "8"), ("float", 8.0), ("zero", 0)):
        case(f"case_{field}_{tag}", "case_invalid", lambda c, f=field, v=value: c["mesh"].update({f: v}))


def raises_callback(*_):
    raise RuntimeError("synthetic evaluator failure")


for tag, callback in (("missing", None), ("noncallable", 42), ("exception", raises_callback),
                      ("nan", lambda *_: math.nan), ("inf", lambda *_: math.inf),
                      ("zero", lambda *_: 0.), ("negative", lambda *_: -.01),
                      ("bool", lambda *_: True), ("string", lambda *_: "0.01")):
    case("callback_" + tag, "callback_invalid", callback)
for side in ("fvm", "fem"):
    for tag, value in (("zero", 0), ("negative", -.1), ("nan", math.nan), ("inf", math.inf), ("bool", True)):
        case(f"{side}_pv_{tag}", "summary_invalid", side,
             lambda m, v=value: m["summary"][0].update(value=v))
    case(side + "_missing_pv", "summary_invalid", side, lambda m: m.update(summary=[]))
    case(side + "_duplicate_pv", "summary_invalid", side,
         lambda m: m["summary"].append(dict(m["summary"][0])))
for tag, method in (("wrong", "FEM top nodes; least-squares plane removal"),
                    ("trailing_space", METHOD + " "), ("lowercase", METHOD.lower()), ("missing", None)):
    case("fvm_method_" + tag, "summary_invalid", "fvm",
         lambda m, v=method: m["summary"][0].update(method=v))

for quantity in ("same", "nodal", "thickness"):
    for d, passes in ((.9999e-3, True), (1.0001e-3, False)):
        case(f"reference_{quantity}_{'below' if passes else 'above'}", "convergence_boundary", quantity, d, passes)
for tag, change in (
        ("missing_thickness", lambda d: d.pop(1)),
        ("missing_initial", lambda d: d.pop(0)),
        ("missing_middle", lambda d: d.pop(2)),
        ("missing_fvm_key", lambda d: d[2]["same_point_pv"].pop(FVM_KEYS[0])),
        ("duplicate_tier", lambda d: d.append(clone(d[0]))),
        ("nan_same", lambda d: d[0]["same_point_pv"].update({FVM_KEYS[0]: math.nan})),
        ("zero_nodal", lambda d: d[0].update(nodal_pv=0)),
        ("empty", lambda d: d.clear())):
    case("reference_invalid_" + tag, "reference_invalid", change)

for nx, kz, error, passes, limit in ((96, 8, .005, True, .005), (96, 8, .0050001, False, .005),
                                    (48, 8, .010, True, .010), (48, 8, .0100001, False, .010),
                                    (96, 4, .015, True, .015), (96, 4, .0151, False, .015),
                                    (48, 4, .030, True, .030), (48, 4, .0300001, False, .030)):
    case(f"gate_absolute_{nx}_{kz}_{'pass' if passes else 'fail'}", "gate_absolute", nx, kz, error, passes, limit)
for axis, target, passes in (("kz", 1.5, True), ("kz", 1.4999, False),
                             ("xy", 1., True), ("xy", .9999, False)):
    case(f"gate_order_{axis}_{'pass' if passes else 'fail'}", "gate_order", axis, target, passes)
for tag, coarse, fine in (("sign_reversal", .012, -.003), ("small_coarse", .001, .0001),
                           ("smaller_coarse", .0009, .0001), ("nondecreasing", .003, .0031),
                           ("equal_errors", .003, .003), ("zero_fine", .012, 0.)):
    case("gate_order_indeterminate_" + tag, "gate_indeterminate", coarse, fine)
for field in ("fvm_curvature", "fem_curvature"):
    for tag, value in (("opposite", .01), ("zero", 0.), ("nan", math.nan), ("inf", math.inf)):
        case(f"gate_{field}_{tag}", "gate_field", field, value)
for tag, value, passes in (("boundary", .05, True), ("above", .0500001, False),
                            ("negative_boundary", -.05, True), ("nan", math.nan, False)):
    case("gate_deformation_" + tag, "gate_field", "deformation_rel", value, passes)
for field in ("same_point_rel", "deformation_rel", "nx", "kz"):
    case("gate_nonfinite_" + field, "gate_field", field, math.nan)
for field in ("nx", "ny", "kz", "same_point_rel", "deformation_rel", "fvm_curvature", "fem_curvature"):
    case("gate_missing_" + field, "gate_missing", field)
for tag, field, value, passes in (("pass", "same_point_rel", .0015, True),
                                  ("absolute", "same_point_rel", .0050001, False),
                                  ("order", "same_point_rel", .003, False),
                                  ("deformation", "deformation_rel", .0500001, False),
                                  ("curvature", "fvm_curvature", .01, False)):
    case("gate_192_" + tag, "gate_192", field, value, passes)
case("gate_192_original_kz4_limit", "gate_192", "same_point_rel", .0151, False, 4)
for tag, kz, same, node, limit, passes in (
        ("kz7", 7, .01, .01, .05, False), ("same_only_fail", 8, .051, .01, .05, False),
        ("node_only_fail", 8, .01, .051, .05, False), ("omitted_node", 8, .05, None, .05, True),
        ("pass", 8, .05, -.05, .05, True), ("caller_08", 8, .08, .08, .08, True),
        ("caller_10", 16, -.1, .1, .1, True), ("nan", 8, math.nan, .01, .05, False)):
    case("preconditions_" + tag, "precondition", kz, same, node, limit, passes)
for name, allowed in (("warpage_pv", True), ("deformation_max", True), ("U_mag_max", False),
                       ("arbitrary", False), ("", False)):
    case("quantity_" + (name or "empty"), "quantity", name, allowed)
for tag, value, platform, expected in (("linux", 1024, "linux", 1.),
                                       ("darwin", 1048576, "darwin", 1.),
                                       ("unsupported", 1024, "win32", None),
                                       ("negative", -1, "linux", None),
                                       ("nan", math.nan, "linux", None),
                                       ("inf", math.inf, "darwin", None)):
    case("rss_" + tag, "rss", value, platform, expected)
VALID_VM = "Pages free: 10.\nPages inactive: 20.\nPages speculative: 3.\n"
for tag, platform, meminfo, vmstat, pagesize, failure in (
        ("linux_missing", "linux", "MemFree: 123 kB\n", "", "4096", None),
        ("linux_malformed", "linux", "MemAvailable: nope kB\n", "", "4096", None),
        ("linux_negative", "linux", "MemAvailable: -1 kB\n", "", "4096", None),
        ("linux_nan", "linux", "MemAvailable: nan kB\n", "", "4096", None),
        ("linux_inf", "linux", "MemAvailable: inf kB\n", "", "4096", None),
        ("linux_read", "linux", "", "", "4096", OSError("synthetic read failure")),
        ("darwin_missing", "darwin", "", "Pages free: 10.\n", "4096", None),
        ("darwin_negative", "darwin", "", VALID_VM.replace("10.", "-1."), "4096", None),
        ("darwin_nan", "darwin", "", VALID_VM.replace("10.", "nan."), "4096", None),
        ("darwin_pagesize", "darwin", "", VALID_VM, "-4096", None),
        ("darwin_pagesize_nan", "darwin", "", VALID_VM, "nan", None),
        ("darwin_read", "darwin", "", VALID_VM, "4096", OSError("synthetic command failure")),
        ("unsupported", "win32", "", "", "4096", None)):
    case("memory_invalid_" + tag, "memory_invalid", platform, meminfo, vmstat, pagesize, failure)


if __name__ == "__main__":
    unittest.main(verbosity=2)
