#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
"""Manual FRD parser regressions; requires the generator's numpy/scipy environment."""
import sys
import io
from contextlib import redirect_stdout
import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))
from tools.vv.calculix_trilayer.generate import build_mesh, ccx_version, parse_frd, top_node_count, w_bfp_from_frd, write_inp  # noqa: E402


class FrdParserTest(unittest.TestCase):
    def test_full_width_node_id(self):
        # All ten ID columns are occupied; negative values abut the ID.
        record = " -11234567890-1.23456E+02 2.34567E-03-3.45678E+01"
        self.assertEqual(len(record), 49)
        self.assertEqual(record[3:13], "1234567890")
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "wide-id.frd"
            path.write_text(
                "  2C\n" + record + "\n -3\n -4  DISP\n" + record + "\n -3\n",
                encoding="ascii",
            )
            nodes, disp = parse_frd(path)
        expected = {1234567890: (-123.456, 0.00234567, -34.5678)}
        self.assertEqual(nodes, expected)
        self.assertEqual(disp, expected)

    def test_checked_in_decks_match_generator(self):
        with tempfile.TemporaryDirectory() as directory:
            for name, n in (("coarse", 8), ("fine", 16)):
                path = Path(directory) / f"{name}.inp"
                write_inp(path, name, n, n)
                self.assertEqual(path.read_bytes(),
                                 Path(__file__).with_name(f"{name}.inp").read_bytes())

    def test_comparison_stdout_and_log(self):
        with tempfile.TemporaryDirectory() as directory:
            for configured in (None, "", str(Path(directory) / "reports")):
                with self.subTest(configured=configured):
                    env = dict(os.environ)
                    env.pop("CHIPCAE_VV_LOG_DIR", None)
                    if configured is not None:
                        env["CHIPCAE_VV_LOG_DIR"] = configured
                    log = Path("/workspace/tasks/logs/v-ccx-delta.md")
                    log.unlink(missing_ok=True)
                    result = subprocess.run(
                        [sys.executable, "-m", "unittest", "tools.vv.test_trilayer_calculix_ref"],
                        cwd=Path(__file__).resolve().parents[3], capture_output=True, text=True,
                        check=True, env=env,
                    )
                    self.assertEqual(len(result.stdout.splitlines()), 1)
                    self.assertTrue(result.stdout.startswith("W_bfp="))
                    self.assertEqual(result.stdout, log.read_text(encoding="utf-8"))
                    self.assertFalse((Path(directory) / "reports/v-ccx-delta.md").exists())

    def test_missing_reference_does_not_report(self):
        from tools.vv import test_trilayer_calculix_ref as comparison

        with tempfile.TemporaryDirectory() as directory:
            missing = Path(directory) / "REFERENCE.json"
            suite = unittest.defaultTestLoader.loadTestsFromTestCase(
                comparison.TrilayerCalculixRefTest
            )
            output = io.StringIO()
            with patch.object(comparison, "REF", missing), \
                    patch.object(Path, "write_text") as write:
                with redirect_stdout(output):
                    result = unittest.TestResult()
                    suite.run(result)
            self.assertFalse(result.wasSuccessful())
            self.assertEqual(len(result.failures), 1)
            self.assertEqual(result.errors, [])
            self.assertIn(f"missing required artifact: {missing}", result.failures[0][1])
            self.assertEqual(output.getvalue(), "")
            write.assert_not_called()

    def test_failed_rejection_checks_do_not_report(self):
        from tools.vv.test_trilayer_calculix_ref import TrilayerCalculixRefTest

        for failure in (AssertionError("injected rejection failure"),
                        RuntimeError("injected rejection error")):
            with self.subTest(failure=type(failure).__name__):
                suite = unittest.defaultTestLoader.loadTestsFromTestCase(TrilayerCalculixRefTest)
                output = io.StringIO()
                with patch.object(TrilayerCalculixRefTest, "check_rejects_invalid_artifacts",
                                  side_effect=failure), patch.object(Path, "write_text") as write:
                    with redirect_stdout(output):
                        result = unittest.TestResult()
                        suite.run(result)
                self.assertFalse(result.wasSuccessful())
                self.assertEqual(len(result.failures) + len(result.errors), 1)
                self.assertEqual(output.getvalue(), "")
                write.assert_not_called()

    def test_deck_model_settings(self):
        # Independent checks of the submitted inputs, without trusting write_inp.
        here = Path(__file__).parent
        reference = json.loads((here / "REFERENCE.json").read_text())
        for mesh in reference["meshes"]:
            blocks = []
            for line in (here / f"{mesh['name']}.inp").read_text().splitlines():
                if line.startswith("**") or not line.strip():
                    continue
                if line.startswith("*"):
                    blocks.append((line, []))
                else:
                    blocks[-1][1].append(line)
            cards = dict(blocks)
            nodes = {}
            for row in cards["*NODE"]:
                nid, *xyz = row.split(",")
                nodes[int(nid)] = tuple(map(float, xyz))
            for name, xyz in (("C321_3", (0, 0, 0)),
                              ("C321_2", (1, 0, 0)), ("C321_1", (0, 1, 0))):
                ids = cards[f"*NSET, NSET={name}"]
                self.assertEqual(len(ids), 1)
                self.assertEqual(nodes[int(ids[0])], xyz)
            self.assertEqual(cards["*BOUNDARY"],
                             ["C321_3, 1, 3", "C321_2, 2, 3", "C321_1, 3, 3"])
            self.assertEqual(cards["*INITIAL CONDITIONS, TYPE=TEMPERATURE"], ["NALL, 400"])
            self.assertEqual(cards["*TEMPERATURE"], ["NALL, 300"])
            self.assertIn("*STATIC", cards)
            self.assertEqual(cards["*NODE FILE, NSET=TOP"], ["U"])
            nx, ny, _ = mesh["n_elem_xyz"]
            self.assertEqual(sorted({p[0] for p in nodes.values()}),
                             [i / (2 * nx) for i in range(2 * nx + 1)])
            self.assertEqual(sorted({p[1] for p in nodes.values()}),
                             [i / (2 * ny) for i in range(2 * ny + 1)])
            materials = [("25000000000, 0.2", "1.5e-05"),
                         ("3e+09, 0.35", "5e-05"), ("1.3e+11, 0.28", "2.6e-06")]
            elastic = [rows for key, rows in blocks if key == "*ELASTIC"]
            expansion = [rows for key, rows in blocks if key == "*EXPANSION, ZERO=400"]
            self.assertEqual(len(elastic), 3)
            self.assertEqual(len(expansion), 3)
            bounds = [(0, .05), (.05, .06), (.06, .1)]
            for i, ((e, alpha), (lo, hi)) in enumerate(zip(materials, bounds), 1):
                self.assertEqual(list(map(float, elastic[i-1][0].split(","))),
                                 list(map(float, e.split(","))))
                self.assertEqual(float(expansion[i-1][0]), float(alpha))
                self.assertIn(f"*SOLID SECTION, ELSET=LAY{i}, MATERIAL=MAT{i}", cards)
                rows = cards[f"*ELEMENT, TYPE=C3D20, ELSET=LAY{i}"]
                self.assertEqual(len(rows), 2 * nx * ny * mesh["n_elem_z_per_layer"][i-1])
                spans = set()
                for j in range(0, len(rows), 2):
                    values = [int(v) for v in (rows[j] + rows[j+1]).split(",") if v.strip()]
                    self.assertEqual(len(values), 21)
                    zs = [nodes[nid][2] for nid in values[1:]]
                    spans.add((min(zs), max(zs)))
                self.assertEqual(spans, {(lo, (lo+hi)/2), ((lo+hi)/2, hi)})

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
        nodes = {1: (0, 0, 0.1), 2: (1, 0, 0.1), 3: (0, 0, 0)}
        for disp in ({1: (0, 0, 1)}, {1: (0, 0, 1), 2: (0, 0, 1)},
                     {1: (0, 0, 1), 3: (0, 0, 1)},
                     {1: (0, 0, 1), 99: (0, 0, 1)}):
            expected = {1, 2} if len(disp) == 1 else {1}
            with patch("tools.vv.calculix_trilayer.generate.parse_frd", return_value=(nodes, disp)):
                with self.assertRaisesRegex(RuntimeError, "expected .* C3D20 top nodes"):
                    w_bfp_from_frd(Path("sample.frd"), expected)

    def test_top_coordinates_must_match_expected(self):
        for nodes in ({1: (0, 0, 0.1), 2: (1, 0, 0.1)},
                      {2: (1, 0, 0.1)}):
            with self.subTest(nodes=nodes), patch(
                "tools.vv.calculix_trilayer.generate.parse_frd",
                return_value=(nodes, {1: (0, 0, 1)}),
            ):
                with self.assertRaisesRegex(RuntimeError, "coordinate"):
                    w_bfp_from_frd(Path("sample.frd"), {1})

    def test_ccx_version_requires_successful_version_output(self):
        for code, stdout, stderr in (
            (0, "This is Version 2.23\n", ""),
            (201, "This is Version 2.23\n", ""),
            (201, "", ""),
            (0, "", "\nThis is Version 2.23\n"),
            (0, "", ""), (0, "unknown", ""),
            (1, "This is Version 2.23", "failed"),
            (0, "This is Version 2.23 extra", ""),
        ):
            with self.subTest(code=code, stdout=stdout, stderr=stderr), patch(
                "tools.vv.calculix_trilayer.generate.subprocess.run",
                return_value=subprocess.CompletedProcess([], code, stdout, stderr),
            ):
                if code in (0, 201) and "This is Version 2.23\n" in stdout + stderr:
                    self.assertEqual(ccx_version(), "This is Version 2.23")
                else:
                    with self.assertRaisesRegex(RuntimeError, "valid version"):
                        ccx_version()

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
