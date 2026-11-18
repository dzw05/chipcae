#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Optional real L1 regeneration plus FRD/solver failure regressions.

Only absent ccx, absent numpy/scipy, or a different ccx version returns 77.
Internal parser contract used by these tests: parse_results(path) returns
{nodes: {id: (x,y,z)}, temperature_blocks: [{id: T}, ...],
displacement_blocks: [{id: (ux,uy,uz)}, ...]}. Blocks remain separate and in
file order, including thermal and mechanical temperatures in a two-step deck.
Each present block must cover the coordinate node set. run_ccx(work, job)
executes one deck by stem, rejects solver failures, and removes stale FRD first.
No production stub is necessary: missing generator is an explicit assertion.
"""

from __future__ import annotations

import importlib
import json
import math
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[3]
ARTIFACT_DIR = ROOT / "tools/vv/calculix_demo_gate"
GENERATOR = ARTIFACT_DIR / "generate.py"
sys.path.insert(0, str(ROOT))
VERSION = None


def dependencies():
    """A broken version query is a failure, not a dependency skip."""
    ccx = os.environ.get("CHIPCAE_CCX", "ccx")
    if shutil.which(ccx) is None:
        print(f"SKIP: ccx executable unavailable: {ccx}")
        return None
    for name in ("numpy", "scipy"):
        try:
            importlib.import_module(name)
        except ModuleNotFoundError as error:
            if error.name != name:
                raise
            print(f"SKIP: missing {name}")
            return None
    result = subprocess.run([ccx, "-v"], capture_output=True, text=True, timeout=30)
    versions = re.findall(r"^\s*(This is Version \d+\.\d+(?:\.\d+)?)\s*$",
                          result.stdout + "\n" + result.stderr, re.MULTILINE)
    if result.returncode not in (0, 201) or len(versions) != 1:
        raise AssertionError(f"ccx -v did not report a unique valid version: exit={result.returncode}, "
                             f"stdout={result.stdout!r}, stderr={result.stderr!r}")
    path = ARTIFACT_DIR / "REFERENCE.json"
    # Missing artifacts must fail in the tests, even with a functioning solver.
    if path.is_file():
        recorded = json.loads(path.read_text(encoding="utf-8"))["calculix_version"]
        if versions[0] != recorded:
            print(f"SKIP: ccx version {versions[0]!r} differs from artifact {recorded!r}")
            return None
    return versions[0]


class DemoGateRegenTest(unittest.TestCase):
    def generator(self):
        self.assertTrue(GENERATOR.is_file(),
                        "missing DEMO-GATE generator: tools/vv/calculix_demo_gate/generate.py")
        return importlib.import_module("tools.vv.calculix_demo_gate.generate")

    def parse(self, text):
        module = self.generator()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "sample.frd"
            path.write_text(text, encoding="ascii")
            return module.parse_results(path)

    @staticmethod
    def frd():
        # Literal FRD short-format records: full-width IDs and abutting negatives.
        return (
            "  2C\n"
            " -1         1 0.00000E+00 0.00000E+00 1.60000E-03\n"
            " -11234567890-1.23456E+02 2.34567E-03-3.45678E+01\n"
            " -3\n"
            " -4  NDTEMP\n"
            " -1         1 3.12345E+02\n"
            " -11234567890 3.00123E+02\n"
            " -3\n"
            " -4  DISP\n"
            " -1         1-1.00000E-03-2.00000E-03-3.00000E-03\n"
            " -11234567890-1.23456E+02 2.34567E-03-3.45678E+01\n"
            " -3\n"
        )

    def test_fixed_width_coordinates_displacement_and_ndtemp(self):
        result = self.parse(self.frd())
        expected = {
            "nodes": {1: (0, 0, 0.0016), 1234567890: (-123.456, 0.00234567, -34.5678)},
            "temperature_blocks": [{1: 312.345, 1234567890: 300.123}],
            "displacement_blocks": [{1: (-0.001, -0.002, -0.003),
                                     1234567890: (-123.456, 0.00234567, -34.5678)}],
        }
        self.assertIsInstance(result, dict)
        for key, value in expected.items():
            if key == "nodes":
                actual_blocks, expected_blocks = [result[key]], [value]
            else:
                actual_blocks, expected_blocks = result[key], value
            self.assertEqual(len(actual_blocks), len(expected_blocks))
            for actual, wanted in zip(actual_blocks, expected_blocks):
                self.assertEqual(set(actual), set(wanted))
                for nid, expected_value in wanted.items():
                    numbers = expected_value if isinstance(expected_value, tuple) else (expected_value,)
                    values = actual[nid] if isinstance(expected_value, tuple) else (actual[nid],)
                    self.assertEqual(len(values), len(numbers))
                    for a, b in zip(values, numbers):
                        self.assertTrue(math.isclose(a, b, rel_tol=1e-12, abs_tol=1e-15))

    def test_results_from_different_steps_are_kept_separate(self):
        # Legal repeated node IDs in distinct result blocks must not be called duplicates.
        text = self.frd() + (
            " -4  NDTEMP\n"
            " -1         1 3.12346E+02\n"
            " -11234567890 3.00124E+02\n"
            " -3\n"
        )
        result = self.parse(text)
        self.assertEqual(len(result["temperature_blocks"]), 2)
        self.assertAlmostEqual(result["temperature_blocks"][0][1], 312.345, delta=1e-12)
        self.assertAlmostEqual(result["temperature_blocks"][1][1], 312.346, delta=1e-12)

    def test_missing_duplicate_incomplete_and_nonfinite_frd_are_rejected(self):
        self.generator()  # Missing implementation must fail outside assertRaises.
        text = self.frd()
        coord = " -11234567890-1.23456E+02 2.34567E-03-3.45678E+01\n"
        temp = " -11234567890 3.00123E+02\n"
        variants = {
            "empty": "",
            "missing coordinates": text[text.index(" -4  NDTEMP"):],
            "missing results": text[:text.index(" -4  NDTEMP")],
            "duplicate coordinate": text.replace(coord, coord * 2, 1),
            "duplicate temperature": text.replace(temp, temp * 2),
            "duplicate displacement": text.rsplit(coord, 1)[0] + coord * 2 + " -3\n",
            "incomplete temperatures": text.replace(temp, ""),
            "unknown temperature node": text.replace(temp, temp.replace("1234567890", "1234567891")),
            "incomplete displacements": text.rsplit(coord, 1)[0] + " -3\n",
            "truncated record": text.replace(" 3.00123E+02", " 3.00123E+"),
            "unterminated block": text[:-4],
            "nonfinite temperature": text.replace(" 3.00123E+02", "         NaN"),
            "nonfinite displacement": text.replace("-3.00000E-03", "         Inf"),
        }
        for name, malformed in variants.items():
            with self.subTest(name=name), self.assertRaises((ValueError, RuntimeError)):
                self.parse(malformed)

    def test_solver_errors_and_stale_frd_do_not_produce_reference(self):
        module = self.generator()
        # Substituting the external executable preserves process launch, output handling,
        # stale-file removal, and failure propagation in the real runner.
        for code, output in ((7, "solver failed"), (0, "*ERROR injected"),
                             (0, "*error injected"), (0, "no FRD written")):
            with self.subTest(code=code, output=output), tempfile.TemporaryDirectory() as directory:
                work = Path(directory)
                fake = work / "fake_ccx"
                fake.write_text(f"#!{sys.executable}\nprint({output!r})\nraise SystemExit({code})\n")
                fake.chmod(0o755)
                (work / "probe.inp").write_text("*HEADING\nsynthetic failure probe\n")
                stale = work / "probe.frd"
                stale.write_text(self.frd())
                with patch.object(module, "CCX", str(fake)):
                    with self.assertRaises((ValueError, RuntimeError)):
                        module.run_ccx(work, "probe")
                self.assertFalse(stale.exists(), "runner must remove stale FRD before launching ccx")
                self.assertFalse((work / "REFERENCE.json").exists())

    def test_version_query_accepts_201_and_requires_a_unique_version(self):
        module = self.generator()
        for code, stdout, stderr, accepted in (
            (201, "This is Version 2.23\n", "", True),
            (0, "", "This is Version 2.23\n", True),
            (201, "", "", False),
            (1, "This is Version 2.23\n", "failed", False),
            (0, "This is Version 2.23\nThis is Version 2.23\n", "", False),
            (0, "This is Version 2.23 extra\n", "", False),
        ):
            with self.subTest(code=code, stdout=stdout, stderr=stderr), patch(
                "subprocess.run", return_value=subprocess.CompletedProcess([], code, stdout, stderr)
            ):
                if accepted:
                    self.assertEqual(module.ccx_version(), "This is Version 2.23")
                else:
                    with self.assertRaises(RuntimeError):
                        module.ccx_version()

    def test_l1_deck_bytes_and_recomputed_qois(self):
        module = self.generator()
        path = ARTIFACT_DIR / "REFERENCE.json"
        self.assertTrue(path.is_file(), "missing required DEMO-GATE REFERENCE.json")
        artifact = json.loads(path.read_text(encoding="utf-8"))
        self.assertEqual(VERSION, artifact["calculix_version"])
        l1 = [level for level in artifact["levels"] if level["name"] == "L1"]
        self.assertEqual(len(l1), 1)
        l1 = l1[0]
        import numpy as np
        from tools.vv.warpage_qoi import best_fit_plane, interp_grid, mirror_quarter, qoi_surface

        with tempfile.TemporaryDirectory(prefix="chipcae-demo-gate-l1-") as directory:
            work = Path(directory)
            result = subprocess.run(
                [sys.executable, str(GENERATOR), "--levels", "L1", "--work", str(work)],
                cwd=ROOT, capture_output=True, text=True, timeout=480,
            )
            self.assertEqual(result.returncode, 0, f"L1 regeneration failed:\n{result.stdout}\n{result.stderr}")
            self.assertNotIn("*ERROR", (result.stdout + result.stderr).upper())
            self.assertFalse((work / "REFERENCE.json").exists(),
                             "L1-only run must not publish a complete three-level reference")
            for deck in l1["decks"]:
                name = deck["file"]
                self.assertEqual(Path(name).name, name)
                self.assertTrue((work / name).is_file(), f"regenerated deck absent: {name}")
                self.assertEqual((work / name).read_bytes(), (ARTIFACT_DIR / name).read_bytes(), name)
            names = [deck["file"] for deck in l1["decks"]]
            if len(names) == 1:
                thermal = mechanical = module.parse_results(work / Path(names[0]).with_suffix(".frd"))
                self.assertGreaterEqual(len(thermal["temperature_blocks"]), 2,
                                        "two-step output must include both temperature fields")
            else:
                self.assertEqual(len(names), 2)
                thermal = module.parse_results(work / "L1_quarter_thermal.frd")
                mechanical = module.parse_results(work / "L1_quarter_mech.frd")
            nodes = thermal["nodes"]
            self.assertEqual(len(nodes), l1["n_nodes"], "FRD must include every mesh node")
            self.assertEqual(set(nodes), set(mechanical["nodes"]))
            temperatures = thermal["temperature_blocks"][0 if len(names) == 1 else -1]
            transferred = mechanical["temperature_blocks"][-1]
            displacement = mechanical["displacement_blocks"][-1]
            for field in (temperatures, transferred, displacement):
                self.assertEqual(set(field), set(nodes))
            self.assertLessEqual(max(abs(temperatures[n] - transferred[n]) for n in nodes), 1e-3)
            top = {n for n, xyz in nodes.items() if abs(xyz[2] - 0.0016) <= 1e-12 * 0.0016}
            self.assertTrue(top, "all top nodes, including midside nodes, must be output")
            # Independently assemble and postprocess FRD data using the required shared QoI helpers.
            points = np.array([(*nodes[n], *displacement[n]) for n in sorted(top)], dtype=float)
            points *= 1000.0
            points = mirror_quarter(points)
            xs = ys = np.linspace(-10.0, 10.0, 81)
            x, y, w = interp_grid(points, xs, ys)
            outside = np.abs(x) + np.abs(y) > 18.0
            self.assertTrue(np.isnan(w[outside]).all(), "chamfer exterior must remain NaN")
            self.assertTrue(np.isfinite(w[~outside]).all())
            qois = {"W_bfp_m": qoi_surface(x, y, w)["W_bfp"] / 1000.0,
                    "T_max_K": max(temperatures.values()),
                    "T_top_min_K": min(temperatures[n] for n in top)}
            _, residual = best_fit_plane(points[:, 0], points[:, 1], points[:, 5])
            self.assertTrue(math.isclose(float(np.ptp(residual)) / 1000.0, l1["W_bfp_nodes_m"],
                                         rel_tol=1e-6, abs_tol=0.0))
            for key, actual in qois.items():
                reference = l1[key]
                denominator = reference if key == "W_bfp_m" else reference - 298.15
                self.assertGreater(abs(denominator), 0)
                self.assertLessEqual(abs(actual - reference) / abs(denominator), 1e-6, key)


if __name__ == "__main__":
    VERSION = dependencies()
    if VERSION is None:
        raise SystemExit(77)
    if len(sys.argv) == 2 and Path(sys.argv[1]).is_dir():
        sys.argv.pop()
    unittest.main(verbosity=2)
