# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
"""Celik 2008 manufactured solutions and the owned-cell grid-size contract."""

from dataclasses import replace
import json
import math
from pathlib import Path
import tempfile
import unittest

from synth import (AMBIENT, crosscheck_module, h5py, mesh_runs, np, synthetic_run)
from tools.vv.field_compare import InputInvalid, compare_runs, load_run


DERIVED = ("p", "phi_ext21", "e_a21", "e_ext21", "gci_fine21")
GCI_KEYS = {"r21", "r32", "eps21", "eps32", "s", "p", "iterations",
            "phi_ext21", "e_a21", "e_ext21", "gci_fine21", "status"}


class SyntheticFixtureTests(unittest.TestCase):
    """Pass independently of the missing feature, proving valid test inputs."""

    def test_three_levels_load_with_prescribed_qois_and_distinct_hashes(self):
        with tempfile.TemporaryDirectory(prefix="crosscheck-fixture-") as tmp:
            runs = [load_run(path) for path in mesh_runs(Path(tmp))]
            self.assertEqual([len(run.cell_gid) for run in runs], [216, 125, 64])
            self.assertEqual(len({run.mesh_sha256 for run in runs}), 3)
            hs = []
            for run in runs:
                self.assertEqual(run.cell_volume.dtype, np.dtype("float64"))
                self.assertTrue(math.isclose(float(np.sum(run.cell_volume)), 1e-6,
                                             abs_tol=0, rel_tol=1e-15))
                h = float((np.sum(run.cell_volume) / len(run.cell_gid)) ** (1 / 3))
                hs.append(h)
                expected = {"T_max": AMBIENT + 2 + 0.5 * h**2,
                            "warpage_pv": 1e-4 * (1 + 0.5 * h**2),
                            "U_mag_max": 2e-4 * (1 + 0.5 * h**2)}
                for key, value in expected.items():
                    self.assertTrue(math.isclose(run.qoi[key], value, rel_tol=1e-15, abs_tol=0))
                    self.assertEqual(run.qoi_method[key], "synthetic")
            self.assertTrue(hs[0] < hs[1] < hs[2])

    def test_two_partitions_and_nonempty_cut_load_all_six_owned_cells(self):
        with tempfile.TemporaryDirectory(prefix="crosscheck-partitions-") as tmp:
            path = Path(tmp) / "partitioned"
            volumes = synthetic_run(path, hex_counts=(2, 1), cut_counts=(1, 2),
                                    volumes=[1e-10, 2e-8, 3e-7, 4e-9, 5e-7, 6e-8])
            run = load_run(path)
            np.testing.assert_array_equal(run.cell_gid, [0, 1, 2, 2**62, 2**62 + 1, 2**62 + 2])
            np.testing.assert_array_equal(run.cell_volume, volumes)
            self.assertEqual(run.points.shape, (48, 3))
            self.assertTrue(np.all(np.isfinite(run.cell_T)))
            with h5py.File(path / "step.vtkhdf", "r") as data:
                np.testing.assert_array_equal(data["VTKHDF/hex/NumberOfCells"][:], [2, 1])
                np.testing.assert_array_equal(data["VTKHDF/cut/NumberOfCells"][:], [1, 2])

    def test_pair_fixtures_support_rigid_removal_and_both_temperature_gate_outcomes(self):
        with tempfile.TemporaryDirectory(prefix="crosscheck-pair-fixture-") as tmp:
            fem, fvm = Path(tmp) / "fem", Path(tmp) / "fvm"
            qoi = {"T_max": AMBIENT + 2.0, "warpage_pv": 1e-4, "U_mag_max": 2e-4}
            synthetic_run(fem, n=8, qoi=qoi)
            for relative, verdict in ((0.005, "pass"), (0.02, "fail")):
                with self.subTest(relative=relative):
                    synthetic_run(fvm, n=8, qoi={**qoi, "T_max": AMBIENT + 2 * (1 + relative)})
                    result = compare_runs(load_run(fem), load_run(fvm), preset="fem-fvm",
                                          t_ambient=AMBIENT, remove_rigid=True)
                    self.assertEqual(result["verdict"], verdict)
                    self.assertIsNotNone(result["fields"]["U_rigid_removed"])


class GciTests(unittest.TestCase):
    def setUp(self):
        self.cc = crosscheck_module()

    def gci(self, h, phi, **options):
        result = self.cc.celik_gci(h, phi, **options)
        self.assertEqual(set(result), GCI_KEYS)
        json.dumps(result, allow_nan=False)
        self.assertIsInstance(result["iterations"], int)
        return result

    def relative(self, actual, expected, tolerance):
        self.assertTrue(math.isclose(actual, expected, rel_tol=tolerance, abs_tol=0),
                        f"{actual!r} != {expected!r} within relative {tolerance}")

    def test_equal_ratio_quadratic_recovers_order_and_extrapolate(self):
        h = (0.010, 0.014, 0.0196)
        phi = [2.0 + 0.5 * x**2 for x in h]
        result = self.gci(h, phi)
        self.assertEqual(result["status"], "monotone")
        self.assertEqual(result["s"], 1)
        self.assertLessEqual(abs(result["p"] - 2), 1e-9)
        self.relative(result["phi_ext21"], 2.0, 1e-12)
        self.relative(result["r21"], 1.4, 1e-15)
        self.relative(result["r32"], 1.4, 1e-15)
        self.assertGreaterEqual(result["iterations"], 1)

    def test_unequal_ratio_fractional_order_recovers_negative_error(self):
        h = (0.010, 0.0137, 0.0193)
        phi = [1.0 - 0.3 * x**1.5 for x in h]
        result = self.gci(h, phi)
        self.assertEqual(result["status"], "monotone")
        self.assertEqual(result["s"], 1)
        self.assertLess(result["eps21"], 0)
        self.assertLess(result["eps32"], 0)
        self.assertLessEqual(abs(result["p"] - 1.5), 1e-8)
        self.relative(result["phi_ext21"], 1.0, 1e-10)
        self.assertGreater(result["iterations"], 1)

    def test_gci_matches_analytic_order_with_independent_safety_factor(self):
        # Binary-exact data isolate the 1e-14 GCI requirement from roundoff in
        # recovering an order from near-equal decimal QoIs in the examples above.
        h = (0.125, 0.25, 0.5)
        phi = (2.0078125, 2.03125, 2.125)  # 2 + 0.5*h**2, analytic p=2
        approximate_error = 0.0234375 / 2.0078125
        denominator = 2.0**2 - 1.0
        for fs in (1.25, 2.0):
            with self.subTest(fs=fs):
                options = {} if fs == 1.25 else {"fs": fs}
                result = self.gci(h, phi, **options)
                self.relative(result["gci_fine21"], fs * approximate_error / denominator, 1e-14)
                self.relative(result["e_a21"], approximate_error, 1e-14)
                self.relative(result["e_ext21"], 0.0078125 / 2.0, 1e-14)
                self.assertEqual(result["iterations"], 1)

    def test_oscillatory_sequence_keeps_signed_status_and_finite_estimate(self):
        result = self.gci((1.0, 2.0, 4.0), (1.0, 1.1, 0.95))
        self.assertEqual(result["status"], "oscillatory")
        self.assertEqual(result["s"], -1)
        self.assertLessEqual(abs(result["p"] - math.log(1.5) / math.log(2)), 1e-12)
        self.assertTrue(math.isfinite(result["gci_fine21"]))

    def test_either_exact_zero_difference_is_flat_with_all_derivatives_null(self):
        for phi in ((1.0, 1.0, 2.0), (1.0, 2.0, 2.0), (1.0, 1.0, 1.0)):
            with self.subTest(phi=phi):
                result = self.gci((1.0, 2.0, 4.0), phi)
                self.assertEqual(result["status"], "flat")
                self.assertEqual(result["iterations"], 0)
                for key in DERIVED:
                    self.assertIsNone(result[key], key)

    def test_small_nonzero_differences_are_not_treated_as_flat(self):
        phi = (0.0, 2.0**-50, 5 * 2.0**-50)
        result = self.gci((1.0, 2.0, 4.0), phi)
        self.assertEqual(result["status"], "monotone")
        self.assertLessEqual(abs(result["p"] - 2), 1e-12)

    def test_one_update_on_unequal_ratios_exhausts_iteration_budget(self):
        h = (0.010, 0.0137, 0.0193)
        result = self.gci(h, [1 - 0.3 * x**1.5 for x in h], max_iter=1)
        self.assertEqual(result["status"], "no_convergence")
        self.assertEqual(result["iterations"], 1)
        for key in DERIVED:
            self.assertIsNone(result[key], key)

    def test_nonpositive_extrapolation_denominator_is_no_convergence(self):
        result = self.gci((1.0, 2.0, 4.0), (1.0, 2.0, 3.0))
        self.assertEqual(result["status"], "no_convergence")
        self.assertIsNone(result["p"])
        self.assertIsNone(result["phi_ext21"])
        self.assertIsNone(result["gci_fine21"])

    def test_finite_data_causing_power_overflow_return_finite_no_convergence_record(self):
        result = self.gci((1.0, 1.01, 1000.0), (0.0, 1.0, 1e300))
        self.assertEqual(result["status"], "no_convergence")
        for key in DERIVED:
            self.assertIsNone(result[key], key)

    def test_zero_fine_qoi_retains_extrapolate_but_null_relative_error_and_gci(self):
        result = self.gci((1.0, 2.0, 4.0), (0.0, 3.0, 15.0))
        self.assertEqual(result["status"], "monotone")
        self.relative(result["phi_ext21"], -1.0, 1e-14)
        self.assertIsNone(result["e_a21"])
        self.assertIsNone(result["gci_fine21"])
        self.relative(result["e_ext21"], 1.0, 1e-14)

    def test_zero_extrapolate_has_no_relative_extrapolation_error(self):
        result = self.gci((1.0, 2.0, 4.0), (1.0, 4.0, 16.0))
        self.assertEqual(result["status"], "monotone")
        self.assertLessEqual(abs(result["phi_ext21"]), 1e-14)
        self.assertIsNone(result["e_ext21"])
        self.relative(result["gci_fine21"], 1.25, 1e-14)

    def test_invalid_lengths_scales_and_nonfinite_inputs_raise_input_invalid(self):
        cases = [((1, 2), (1, 4, 16)), ((1, 2, 4), (1, 4)),
                 ((0, 2, 4), (1, 4, 16)), ((-1, 2, 4), (1, 4, 16)),
                 ((1, 1, 4), (1, 4, 16)), ((4, 2, 1), (1, 4, 16))]
        for value in (math.nan, math.inf, -math.inf):
            for index in range(3):
                h, phi = [1, 2, 4], [1, 4, 16]
                h[index] = value
                cases.append((h, phi))
                h, phi = [1, 2, 4], [1, 4, 16]
                phi[index] = value
                cases.append((h, phi))
        for h, phi in cases:
            with self.subTest(h=h, phi=phi), self.assertRaises(InputInvalid):
                self.cc.celik_gci(h, phi)

    def test_invalid_iteration_controls_raise_input_invalid(self):
        cases = [{"fs": 0}, {"fs": -1}, {"fs": math.inf}, {"fs": math.nan},
                 {"tol": 0}, {"tol": -1}, {"tol": math.inf}, {"tol": math.nan},
                 {"max_iter": 0}, {"max_iter": -1}, {"max_iter": 1.5}]
        for options in cases:
            with self.subTest(options=options), self.assertRaises(InputInvalid):
                self.cc.celik_gci((1, 2, 4), (1, 4, 16), **options)

    def test_grid_size_includes_hex_cut_and_both_partitions_with_exact_sum_order(self):
        with tempfile.TemporaryDirectory(prefix="crosscheck-grid-") as tmp:
            path = Path(tmp) / "partitioned"
            volumes = synthetic_run(path, hex_counts=(2, 1), cut_counts=(1, 2),
                                    volumes=[1e-10, 2e-8, 3e-7, 4e-9, 5e-7, 6e-8])
            n, h = self.cc.grid_size(load_run(path))
            self.assertEqual(n, 6)
            expected = float((np.sum(volumes, dtype=np.float64) / 6) ** (1 / 3))
            self.assertEqual(float(h).hex(), expected.hex())  # brief requires bitwise equality

    def test_grid_size_rejects_missing_volume_and_nonfinite_computed_scale(self):
        with tempfile.TemporaryDirectory(prefix="crosscheck-volume-") as tmp:
            path = Path(tmp) / "run"
            synthetic_run(path, n=2)
            run = load_run(path)
            for volumes in (None, np.array([np.finfo(float).max] * 2)):
                with self.subTest(volumes=volumes), self.assertRaises(InputInvalid):
                    self.cc.grid_size(replace(run, cell_volume=volumes))


if __name__ == "__main__":
    # CTest's source-dir argument is not a unittest selector.
    unittest.main(argv=[__file__], verbosity=2)
