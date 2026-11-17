# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
"""CLT magnitude gate for the WP-2 trilayer (reference values, not measured).

Compares classical laminate theory (Hsueh, biaxial modulus) against the
prototype MFEM+MUMPS reference 4.269532769e-3 m. This is a 5% sign/magnitude
guard only. The existing 1e-6 solver regression in tests/solve/test_pipeline.py
is untouched.
"""
from __future__ import annotations

import unittest

from tools.vv.derive_cards import hsueh

PROTOTYPE_W_BFP_M = 4.269532769e-3  # 参考值（原型 MFEM+MUMPS 计算值，不是解析解，不是实测）


class TrilayerCltMagnitudeTest(unittest.TestCase):
    def test_clt_within_five_percent_of_prototype(self):
        # Bottom-to-top: substrate, attach, silicon. dT = T_target - T_ref = -100 K.
        layers = [
            (25e9, 0.2, 0.05, 15e-6),
            (3e9, 0.35, 0.01, 50e-6),
            (130e9, 0.28, 0.04, 2.6e-6),
        ]
        ebar = [(E / (1.0 - nu), t, alpha) for E, nu, t, alpha in layers]
        _c, _b, kappa = hsueh(ebar[0], ebar[1:], dT=-100.0)
        L = 1.0
        w_bfp = abs(kappa) * L * L / 4.0
        rel = abs(w_bfp - PROTOTYPE_W_BFP_M) / PROTOTYPE_W_BFP_M
        print(
            "trilayer CLT kappa=",
            kappa,
            "1/m W_bfp=",
            w_bfp,
            "m prototype_ref=",
            PROTOTYPE_W_BFP_M,
            "m relative_diff=",
            rel,
            "(参考值：原型 MFEM+MUMPS 计算值，不是解析解，不是实测)",
            flush=True,
        )
        self.assertGreater(kappa, 0.0, "cooling should produce positive Hsueh curvature (center high)")
        self.assertLessEqual(rel, 0.05, f"CLT vs prototype relative difference {rel} exceeds 5%")


if __name__ == "__main__":
    unittest.main()
