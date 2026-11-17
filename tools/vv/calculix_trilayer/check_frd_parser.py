#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
"""Manual FRD parser regressions; requires the generator's numpy/scipy environment."""
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))
from tools.vv.calculix_trilayer.generate import build_mesh, parse_frd, top_node_count, w_bfp_from_frd, write_inp  # noqa: E402


class FrdParserTest(unittest.TestCase):
    def test_checked_in_decks_match_generator(self):
        with tempfile.TemporaryDirectory() as directory:
            for name, n in (("coarse", 8), ("fine", 16)):
                path = Path(directory) / f"{name}.inp"
                write_inp(path, name, n, n)
                self.assertEqual(path.read_bytes(),
                                 Path(__file__).with_name(f"{name}.inp").read_bytes())

    def test_unsupported_and_incomplete_records_fail(self):
        valid = " -1         1 0.00000E+00 1.00000E+00 1.00000E-01"
        for record in (valid + " extra", valid.replace("E+00", "D+00"),
                       valid[:40], valid + "\n" + valid):
            with self.subTest(record=record), tempfile.TemporaryDirectory() as directory:
                path = Path(directory) / "bad.frd"
                path.write_text("  2C\n" + record + "\n -3\n", encoding="ascii")
                with self.assertRaises(ValueError):
                    parse_frd(path)

    def test_mesh_has_only_connected_nodes(self):
        for nx, expected in ((8, 225), (16, 833)):
            with self.subTest(nx=nx):
                coords, elements, _ = build_mesh(nx, nx)
                used = {nid for _, _, nodes in elements for nid in nodes}
                self.assertEqual(set(coords), used)
                self.assertEqual(sorted(coords), list(range(1, len(coords) + 1)))
                top = {nid for nid, (_, _, z) in coords.items() if z >= 0.1 - 1e-13}
                self.assertEqual(len(top), expected)
                self.assertEqual(len(top), top_node_count(nx, nx))
                self.assertTrue(top <= used)

    def test_incomplete_or_extra_top_displacements_fail(self):
        nodes = {1: (0, 0, 0.1), 2: (1, 0, 0.1)}
        for disp in ({1: (0, 0, 1)}, {1: (0, 0, 1), 2: (0, 0, 1)}):
            expected = {1, 2} if len(disp) == 1 else {1}
            with patch("tools.vv.calculix_trilayer.generate.parse_frd", return_value=(nodes, disp)):
                with self.assertRaisesRegex(RuntimeError, "expected .* C3D20 top nodes"):
                    w_bfp_from_frd(Path("sample.frd"), expected)

    def test_coordinate_and_displacement_records(self):
        # Literal CalculiX ASCII short-format records, including abutting negatives.
        text = (
            "  2C\n"
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
