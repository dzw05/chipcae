# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
"""Analytic convergence orders, reference disagreement and argument precedence."""
import inspect
import math
import sys
import unittest

from q2_synth import DUAL_KEYS, RICHARDSON_KEYS, assert_prefix
from tools.vv import fem_q2_point_ref as ref


class RichardsonDualTests(unittest.TestCase):
    def error(self, prefix, function, *args, **kwargs):
        assert_prefix(self, ref.Q2RefError, prefix, function, *args, **kwargs)

    def test_monotone_analytic_orders_and_gci(self):
        for p in (2, 3):
            for coefficient in (0.75, -0.75):
                with self.subTest(p=p, coefficient=coefficient):
                    f0, fs = 2.0, 1.25
                    coarse, medium, fine = [f0 + coefficient*h**p for h in (1, 0.5, 0.25)]
                    result = ref.richardson(coarse, medium, fine)
                    self.assertEqual(set(result), RICHARDSON_KEYS)
                    self.assertEqual(result["status"], "MONOTONE")
                    self.assertLessEqual(abs(result["R"]-2.0**(-p)), 1e-12*2.0**(-p))
                    self.assertLessEqual(abs(result["p"]-p), 1e-10*p)
                    self.assertLessEqual(abs(result["f_ext"]-f0), 1e-12*abs(f0))
                    gci = fs*abs((fine-medium)/fine)/(2**p-1)
                    self.assertLessEqual(abs(result["gci_fine"]-gci), 1e-12*gci)
                    for key in ("R", "p", "f_ext", "gci_fine"):
                        self.assertTrue(math.isfinite(result[key]))

    def test_explicit_ratio_and_safety_factor(self):
        f0, coefficient, p, ratio, fs = 4.0, 2.0, 3, 3.0, 1.5
        coarse, medium, fine = [f0+coefficient*h**p for h in (1, 1/ratio, 1/ratio**2)]
        result = ref.richardson(coarse, medium, fine, ratio=ratio, fs=fs)
        self.assertEqual(result["status"], "MONOTONE")
        self.assertLessEqual(abs(result["p"]-p), 1e-10*p)
        self.assertLessEqual(abs(result["f_ext"]-f0), 1e-12*f0)
        gci = fs*abs((fine-medium)/fine)/(ratio**p-1)
        self.assertLessEqual(abs(result["gci_fine"]-gci), 1e-12*gci)

    def test_monotone_zero_fine_preserves_order_and_extrapolate(self):
        # f(h) = -1 + (ratio**2 * h)**p at h = 1, 1/ratio, 1/ratio**2;
        # negating the sequence changes only the extrapolate's sign.
        cases = [((15, 3, 0.0), 2, 2, 0.25, -1),
                 ((-15, -3, 0.0), 2, 2, 0.25, 1),
                 ((63, 7, -0.0), 2, 3, 0.125, -1),
                 ((-63, -7, -0.0), 2, 3, 0.125, 1),
                 ((80, 8, 0.0), 3, 2, 1/9, -1),
                 ((-80, -8, -0.0), 3, 2, 1/9, 1)]
        for values, ratio, p, expected_R, f_ext in cases:
            with self.subTest(values=values, ratio=ratio):
                result = ref.richardson(*values, ratio=ratio)
                self.assertEqual(set(result), RICHARDSON_KEYS)
                self.assertEqual(result["status"], "MONOTONE")
                self.assertLessEqual(abs(result["R"]-expected_R), 1e-12*expected_R)
                self.assertLessEqual(abs(result["p"]-p), 1e-10*p)
                self.assertLessEqual(abs(result["f_ext"]-f_ext), 1e-12*abs(f_ext))
                for key in ("R", "p", "f_ext"):
                    self.assertTrue(math.isfinite(result[key]))
                self.assertIsNone(result["gci_fine"])

    def test_nonmonotone_and_indeterminate_states(self):
        cases = [((1, 1.1, 1.05), "OSCILLATORY", -0.5),
                 ((1, 1.1, 1.3), "DIVERGENT", 2.0),
                 ((1, 2, 3), "DIVERGENT", 1.0),
                 ((1, 1, 1.2), "INDETERMINATE", None),
                 ((1, 1.2, 1.2), "INDETERMINATE", 0.0),
                 ((1, 1, 1), "INDETERMINATE", None)]
        for values, status, expected_R in cases:
            with self.subTest(values=values):
                result = ref.richardson(*values)
                self.assertEqual(set(result), RICHARDSON_KEYS)
                self.assertEqual(result["status"], status)
                if expected_R is None:
                    self.assertIsNone(result["R"])
                else:
                    self.assertIsNotNone(result["R"])
                    self.assertLessEqual(abs(result["R"]-expected_R), 1e-14)
                for key in ("p", "f_ext", "gci_fine"):
                    self.assertIsNone(result[key])

    def test_dual_states_and_exact_relative_error_arithmetic(self):
        cases = [(1.0, 1.01, 0.99, 0.02, "PASS", True, True),
                 (1.0, 2.0, 4.0, 0.02, "FAIL", False, False),
                 (1.0, 1.01, 2.0, 0.02, "REFERENCE-DISAGREE", True, False),
                 (1.0, 2.0, 1.01, 0.02, "REFERENCE-DISAGREE", False, True)]
        for value, exact, rich, tol, status, pass_exact, pass_rich in cases:
            with self.subTest(status=status, pass_exact=pass_exact):
                result = ref.dual_reference(value, exact, rich, tol)
                self.assertEqual(set(result), DUAL_KEYS)
                self.assertIs(type(result["e_exact"]), float)
                self.assertIs(type(result["e_richardson"]), float)
                # The brief requires this arithmetic bit for bit, not (value-ref)/ref.
                self.assertEqual(result["e_exact"].hex(), (value/exact-1).hex())
                self.assertEqual(result["e_richardson"].hex(), (value/rich-1).hex())
                self.assertIs(result["pass_exact"], pass_exact)
                self.assertIs(result["pass_richardson"], pass_rich)
                self.assertEqual(result["status"], status)

    def test_dual_tolerance_includes_both_exactly_representable_boundaries(self):
        for value in (0.875, 1.125):
            result = ref.dual_reference(value, 1.0, 1.0, 0.125)
            self.assertEqual(result["status"], "PASS")
            self.assertIs(result["pass_exact"], True)
            self.assertIs(result["pass_richardson"], True)
        result = ref.dual_reference(1.125, 1, 1, math.nextafter(0.125, 0))
        self.assertEqual(result["status"], "FAIL")

    def test_dual_requires_explicit_tol(self):
        signature = inspect.signature(ref.dual_reference)
        self.assertIs(signature.parameters["tol"].default, inspect.Parameter.empty)
        with self.assertRaisesRegex(TypeError, r"\btol\b"):
            ref.dual_reference(1, 1, 1)
        with self.assertRaisesRegex(TypeError, r"\btol\b"):
            ref.dual_reference(fvm_pv=1, exact_point_pv=1, richardson_pv=1)
        result = ref.dual_reference(1, 1, 1, tol=0.1)
        self.assertEqual(result["status"], "PASS")

    def test_rejects_finite_invalid_parameters(self):
        for args in ((1, 1, 1, 0), (1, 1, 1, -0.1), (1, 0, 1, 0.1), (1, 1, 0, 0.1)):
            self.error("Q2REF_BAD_ARG", ref.dual_reference, *args)
        for ratio, fs in ((1, 1.25), (0, 1.25), (-2, 1.25), (2, 0), (2, -1)):
            self.error("Q2REF_BAD_ARG", ref.richardson, 2, 1.5, 1.25, ratio, fs)

    def test_all_numeric_parameters_reject_nonfinite(self):
        for function, base in ((ref.richardson, [2, 1.5, 1.25, 2, 1.25]),
                               (ref.dual_reference, [1, 1, 1, 0.1])):
            for index in range(len(base)):
                for value in (float("nan"), float("inf"), -float("inf")):
                    with self.subTest(function=function.__name__, index=index, value=value):
                        args = base.copy()
                        args[index] = value
                        self.error("Q2REF_NONFINITE", function, *args)

    def test_type_then_finiteness_then_range(self):
        for function, args in ((ref.richardson, [None, float("nan"), 1, 0, 1]),
                               (ref.dual_reference, [None, float("nan"), 0, 0])):
            self.error("Q2REF_BAD_ARG", function, *args)
        for args in ((2, 1.5, float("nan"), 1, 0), (2, 1.5, 1.25, 1, float("inf"))):
            self.error("Q2REF_NONFINITE", ref.richardson, *args)
        for args in ((1, float("inf"), 0, -1), (1, 0, 1, float("nan"))):
            self.error("Q2REF_NONFINITE", ref.dual_reference, *args)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
