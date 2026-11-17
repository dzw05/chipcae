#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
"""Manual FRD parser regressions; requires the generator's numpy/scipy environment."""
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))
from tools.vv.calculix_trilayer.generate import parse_frd  # noqa: E402


class FrdParserTest(unittest.TestCase):
    def test_coordinate_and_displacement_records(self):
        # Literal CalculiX ASCII short-format records, including abutting negatives.
        text = (
            "    2C\n"
            " -1         1 0.00000E+00 1.00000E+00 1.00000E-01\n"
            " -1     12345-1.23456E+02-2.34567E-03 3.45678E+01\n"
            " -3\n"
            " -1         9 9.00000E+00 9.00000E+00 9.00000E+00\n"
            " -4  DISP\n"
            " -1         1-1.00000E-03-2.00000E-03-3.00000E-03\n"
            " -1     12345 1.23456E+02 2.34567E-03-3.45678E+01\n"
            " -3\n"
        )
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "sample.frd"
            path.write_text(text, encoding="ascii")
            nodes, disp = parse_frd(path)
        self.assertEqual(nodes, {1: (0.0, 1.0, 0.1), 12345: (-123.456, -0.00234567, 34.5678)})
        self.assertEqual(disp, {1: (-0.001, -0.002, -0.003), 12345: (123.456, 0.00234567, -34.5678)})


if __name__ == "__main__":
    unittest.main()
