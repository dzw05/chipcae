#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Optional real L1 solves plus solver-free deck/FRD/failure regressions.

Only absent ccx, absent numpy/scipy, or a different ccx version skips tests.
All applicable regressions run before returning 77 for dependency skips, and
failures take precedence. Missing reference artifacts fail artifact comparisons.
The CMake generator configuration defers artifact comparisons only while no
artifact is published; all generator regressions remain enabled.
Internal parser contract used by these tests: parse_results(path) returns
{nodes: {id: (x,y,z)}, temperature_blocks: [{id: T}, ...],
displacement_blocks: [{id: (ux,uy,uz)}, ...]}. Blocks remain separate and in
file order, including thermal and mechanical temperatures in a two-step deck.
Each present block must cover the coordinate node set. run_ccx(work, job)
executes one deck by stem, rejects solver failures, and removes stale FRD first.
No production stub is necessary: missing generator is an explicit assertion.
"""

from __future__ import annotations

import copy
import hashlib
import importlib
import importlib.util
import json
import math
import os
import re
import shutil
import struct
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
    if path.is_file():
        artifact = json.loads(path.read_text(encoding="utf-8"))
        recorded = artifact.get("calculix_version") if isinstance(artifact, dict) else None
        if not isinstance(recorded, str) or not re.fullmatch(
            r"This is Version \d+\.\d+(?:\.\d+)?", recorded
        ):
            raise AssertionError("invalid artifact calculix_version: expected a ccx version string")
        if versions[0] != recorded:
            print(f"SKIP: ccx version {versions[0]!r} differs from required {recorded!r}")
            return None
    return versions[0]


class DemoGateRegenTest(unittest.TestCase):
    def setUp(self):
        if self._testMethodName == "test_l1_deck_bytes_and_recomputed_qois":
            self.assertTrue((ARTIFACT_DIR / "REFERENCE.json").is_file(),
                            "missing required DEMO-GATE REFERENCE.json")
        if self._testMethodName in (
            "test_l1_generator_is_deterministic_without_publishing_reference",
            "test_l1_deck_bytes_and_recomputed_qois",
            "test_refined_iterative_mechanics_agrees_with_direct_solve",
        ) and dependencies() is None:
            self.skipTest("real L1 solve dependencies unavailable; see dependency diagnostic")

    def test_generator_configuration_defers_only_absent_artifact_comparisons(self):
        comparisons = {"test_l2_l3_deck_hashes_match_recorded_values_without_solver",
                       "test_l1_deck_bytes_and_recomputed_qois"}
        with tempfile.TemporaryDirectory() as directory, patch(
            __name__ + ".ARTIFACT_DIR", Path(directory)
        ):
            for mode, present in ((None, False), ("generator", False), ("generator", True),
                                  ("reference", False), ("reference", True)):
                with self.subTest(mode=mode, present=present), patch.dict(
                    os.environ
                ):
                    os.environ.pop("CHIPCAE_DEMO_GATE_TEST_MODE", None)
                    if mode is not None:
                        os.environ["CHIPCAE_DEMO_GATE_TEST_MODE"] = mode
                    path = Path(directory) / "REFERENCE.json"
                    if present:
                        path.write_text('{"status": "参考不合格"}', encoding="utf-8")
                    else:
                        path.unlink(missing_ok=True)
                    suite = unittest.TestLoader().loadTestsFromModule(sys.modules[__name__])
                    names = {test._testMethodName for group in suite for test in group}
                    expected = set(unittest.TestLoader().getTestCaseNames(DemoGateRegenTest))
                    if mode == "generator" and not present:
                        expected -= comparisons
                    self.assertEqual(names, expected)

    def test_missing_artifact_fails_comparisons_before_dependency_skips(self):
        comparisons = {"test_l2_l3_deck_hashes_match_recorded_values_without_solver",
                       "test_l1_deck_bytes_and_recomputed_qois"}
        with tempfile.TemporaryDirectory() as directory, patch(
            __name__ + ".ARTIFACT_DIR", Path(directory)
        ), patch.dict(os.environ, {"CHIPCAE_DEMO_GATE_TEST_MODE": "reference"}), patch.object(
            shutil, "which", return_value=None
        ):
            suite = unittest.TestLoader().loadTestsFromModule(sys.modules[__name__])
            checks = unittest.TestSuite(test for group in suite for test in group
                                        if test._testMethodName in comparisons)
            result = unittest.TestResult()
            checks.run(result)
            self.assertEqual(result.testsRun, 2)
            self.assertEqual(len(result.failures), 2)
            self.assertFalse(result.errors or result.skipped)

    def assert_regenerated_deck_hashes(self, level):
        module = self.generator()
        inputs = module.load_inputs()
        with tempfile.TemporaryDirectory(prefix="chipcae-demo-gate-decks-") as directory:
            paths = module.write_decks(Path(directory), f"{level['name']}_quarter",
                                       module.build_mesh(inputs, level["name"]), inputs)
            self.assertCountEqual([deck["file"] for deck in level["decks"]],
                                  [path.name for path in paths])
            recorded = {deck["file"]: deck["sha256"] for deck in level["decks"]}
            for path in paths:
                self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(), recorded[path.name],
                                 f"sha256 mismatch: {path.name}")

    def test_l2_l3_deck_hashes_match_recorded_values_without_solver(self):
        path = ARTIFACT_DIR / "REFERENCE.json"
        self.assertTrue(path.is_file(), "missing required DEMO-GATE REFERENCE.json")
        artifact = json.loads(path.read_text(encoding="utf-8"))
        for name in ("L2", "L3"):
            with self.subTest(level=name):
                levels = [level for level in artifact["levels"] if level["name"] == name]
                self.assertEqual(len(levels), 1)
                self.assert_regenerated_deck_hashes(levels[0])

    def test_l2_l3_deck_hash_comparison_rejects_arbitrary_digests(self):
        # Deck-byte fixtures from the recorded qualification attempt, with no
        # solver results or claim of reference qualification.
        hashes = {
            "L2": ("c6ffbcf2008a072c3516fc91e79a24781106a1eab663579e2074bf68c9273932",
                   "ac563ae4fdedaeae18b47c59126d1112206d17e04dac003c880351aca5586936"),
            "L3": ("0b3b72e8b8ce33c86cd379b56b47cd8789f0f7d50795881e5b2ff89a2dc4713b",
                   "82f2855c20ac7290a47f5b81340d9cc98239beecec80b9875fa8ba7ade20e908"),
        }
        for name, digests in hashes.items():
            level = {"name": name, "decks": [
                {"file": f"{name}_quarter_{kind}.inp", "sha256": digest}
                for kind, digest in zip(("thermal", "mech"), digests)
            ]}
            with self.subTest(level=name):
                self.assert_regenerated_deck_hashes(level)
            for index in range(len(level["decks"])):
                with self.subTest(level=name, deck=level["decks"][index]["file"]):
                    bad = copy.deepcopy(level)
                    bad["decks"][index]["sha256"] = "0" * 64
                    with self.assertRaisesRegex(AssertionError, "sha256 mismatch"):
                        self.assert_regenerated_deck_hashes(bad)

    def test_l1_generator_is_deterministic_without_publishing_reference(self):
        from tests.vv.calculix_demo_gate.test_reference_json import DemoGateReferenceTest

        module = self.generator()
        with tempfile.TemporaryDirectory(prefix="chipcae-demo-gate-generator-") as directory:
            work = Path(directory)
            result = subprocess.run(
                [sys.executable, str(GENERATOR), "--levels", "L1", "--work", str(work)],
                cwd=ROOT, capture_output=True, text=True, timeout=480,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertNotIn("*ERROR", (result.stdout + result.stderr).upper())
            self.assertFalse((work / "REFERENCE.json").exists())
            candidate = json.loads((work / "L1_quarter.json").read_text())
            # Validate real solver evidence independently of the generator;
            # a single level does not establish reference qualification.
            self.assertTrue(DemoGateReferenceTest().evidence(candidate, 0.25))
            inputs = module.load_inputs()
            regenerated = work / "regenerated"
            regenerated.mkdir()
            decks = module.write_decks(regenerated, "L1_quarter",
                                       module.build_mesh(inputs, "L1"), inputs)
            for deck in decks:
                self.assertEqual(deck.read_bytes(), (work / deck.name).read_bytes(), deck.name)
            self.assertEqual(candidate["n_nodes"], 3046)
            self.assertEqual(candidate["n_elements"], 594)
            self.assertLessEqual(candidate["heat_source"]["relative_error"], 1e-10)
            self.assertLessEqual(candidate["temperature_transfer"]["max_abs_delta_K"], 1e-3)
            for key, lower in (("W_bfp_m", 0), ("T_max_K", 298.15), ("T_top_min_K", 298.15)):
                self.assertTrue(math.isfinite(candidate[key]))
                self.assertGreater(candidate[key], lower)

    def test_generated_artifact_matches_distinct_validator_schema(self):
        from tests.vv.calculix_demo_gate.test_reference_json import ValidatorContractTest

        fixture = ValidatorContractTest()
        fixture.setUp()
        self.addCleanup(fixture.doCleanups)
        data = fixture.load_artifact()
        module = self.generator()
        # Replace only the expensive external-model solves. Input loading,
        # assessment, provenance, serialization and independent validation are real.
        models = {(level["name"], False, False): level for level in data["levels"]}
        models[("L1", True, False)] = data["symmetry_check"]
        models[("L3", False, True)] = data["element_cross"]

        def model_result(work, name, inputs, full=False, complete=False):
            return copy.deepcopy(models[(name, full, complete)])

        with patch.object(sys, "argv", [str(GENERATOR), "--work", str(fixture.directory)]), patch.object(
            module, "ccx_version", return_value="This is Version 2.23"
        ), patch.object(module, "run_model", side_effect=model_result):
            module.main()
        artifact = json.loads((fixture.directory / "REFERENCE.json").read_text())
        fixture.validate_artifact(artifact)

    def test_refined_iterative_mechanics_agrees_with_direct_solve(self):
        module = self.generator()
        inputs = module.load_inputs()
        with tempfile.TemporaryDirectory(prefix="chipcae-demo-gate-refinement-") as directory:
            work = Path(directory)
            direct = module.run_model(work, "L1", inputs)
            direct_displacement = module.parse_results(work / "L1_quarter_mech.frd")["displacement_blocks"][-1]
            mesh = module.build_mesh(inputs, "L1")
            decks = module.write_decks(work, "L1_quarter", mesh, inputs, iterative=True)
            module.run_ccx(work, decks[1].stem)
            thermal = module.parse_results(decks[0].with_suffix(".frd"))
            mechanical = module.parse_results(decks[1].with_suffix(".frd"))
            # The unloaded initialization must not deform or heat the model.
            self.assertGreaterEqual(len(mechanical["displacement_blocks"]), 3)
            self.assertLessEqual(max(abs(v) for xyz in mechanical["displacement_blocks"][0].values()
                                     for v in xyz), 1e-15)
            self.assertLessEqual(max(abs(t - 298.15) for t in mechanical["temperature_blocks"][0].values()),
                                 1e-12)
            refined = module.compute_qoi(thermal, mechanical, mesh, inputs, work, "refined")
            self.assertLessEqual(abs(refined["W_bfp_m"] - direct["W_bfp_m"]) / direct["W_bfp_m"],
                                 1e-6)
            self.assertLessEqual(refined["temperature_transfer"]["max_abs_delta_K"], 1e-3)
            final_displacement = mechanical["displacement_blocks"][-1]
            scale = max(abs(v) for xyz in direct_displacement.values() for v in xyz)
            self.assertLessEqual(max(abs(a - b) for n in direct_displacement
                                     for a, b in zip(final_displacement[n], direct_displacement[n])) / scale,
                                 1e-6)

    def test_iterative_nonconvergence_with_fresh_frd_is_rejected(self):
        module = self.generator()
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            fake = work / "unconverged_ccx"
            fake.write_text(f"#!{sys.executable}\nfrom pathlib import Path\n"
                            "Path('probe.frd').write_text('fresh but unconverged')\n"
                            "print('error condition (0=good, 1=bad) = 1')\n")
            fake.chmod(0o755)
            with patch.object(module, "CCX", str(fake)):
                with self.assertRaisesRegex(RuntimeError, "iterative solver did not converge"):
                    module.run_ccx(work, "probe")
            self.assertTrue((work / "probe.failure.json").is_file())

    def test_missing_and_invalid_artifacts_fail(self):
        spec = importlib.util.spec_from_file_location(
            "demo_gate_reference", Path(__file__).with_name("test_reference_json.py"))
        reference = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(reference)
        with tempfile.TemporaryDirectory() as directory:
            artifact_dir = Path(directory)
            checks = (reference.DemoGateReferenceTest().load_artifact,
                      self.test_l1_deck_bytes_and_recomputed_qois)
            with patch.object(reference, "ARTIFACT_DIR", artifact_dir), patch(
                __name__ + ".ARTIFACT_DIR", artifact_dir
            ):
                for check in checks:
                    with self.subTest(check=check.__name__, artifact="absent"):
                        try:
                            with self.assertRaises(AssertionError):
                                check()
                        except unittest.SkipTest:
                            self.fail("a missing reference must fail acceptance, never skip")
                path = artifact_dir / "REFERENCE.json"
                path.mkdir()
                for check in checks:
                    with self.subTest(check=check.__name__, artifact="directory"):
                        with self.assertRaises(AssertionError):
                            check()
                path.rmdir()
                path.write_text("{", encoding="utf-8")
                for check in checks:
                    with self.subTest(check=check.__name__, artifact="malformed"):
                        with self.assertRaises(json.JSONDecodeError):
                            check()
                path.write_text('{"status": "参考不合格"}', encoding="utf-8")
                with self.assertRaises(AssertionError):
                    reference.DemoGateReferenceTest().validate_artifact(checks[0]())
                with self.assertRaises(KeyError):
                    checks[1]()

    def test_dependency_skip_and_failure_matrix(self):
        with tempfile.TemporaryDirectory() as directory, patch(
            __name__ + ".ARTIFACT_DIR", Path(directory)
        ), patch.object(shutil, "which", return_value="synthetic-ccx"), patch.object(
            importlib, "import_module"
        ), patch.object(subprocess, "run") as query:
            query.return_value = subprocess.CompletedProcess(
                [], 201, "This is Version 2.23\n", "")
            # A functioning solver must not hide a missing artifact behind a dependency skip.
            self.assertEqual(dependencies(), "This is Version 2.23")
            query.return_value = subprocess.CompletedProcess(
                [], 0, "This is Version 2.24\n", "")
            self.assertEqual(dependencies(), "This is Version 2.24")
            query.return_value = subprocess.CompletedProcess(
                [], 201, "This is Version 2.23\n", "")
            with patch.object(shutil, "which", return_value=None):
                self.assertIsNone(dependencies())
            for name in ("numpy", "scipy"):
                def missing(module_name):
                    if module_name == name:
                        raise ModuleNotFoundError(name=name)
                with self.subTest(dependency=name), patch.object(
                    importlib, "import_module", side_effect=missing
                ):
                    self.assertIsNone(dependencies())
            path = Path(directory) / "REFERENCE.json"
            for artifact in ({}, {"calculix_version": None}, {"calculix_version": 223},
                             {"calculix_version": ""}, {"calculix_version": "garbage"}):
                with self.subTest(artifact=artifact):
                    path.write_text(json.dumps(artifact), encoding="utf-8")
                    with self.assertRaisesRegex(AssertionError, "calculix_version"):
                        dependencies()
            path.write_text('{"calculix_version": "This is Version 2.23"}', encoding="utf-8")
            self.assertEqual(dependencies(), "This is Version 2.23")
            query.return_value = subprocess.CompletedProcess(
                [], 0, "This is Version 2.24\n", "")
            self.assertIsNone(dependencies())
            query.return_value = subprocess.CompletedProcess([], 1, "", "broken query")
            with self.assertRaises(AssertionError):
                dependencies()

    def generator(self):
        self.assertTrue(GENERATOR.is_file(),
                        "missing DEMO-GATE generator: tools/vv/calculix_demo_gate/generate.py")
        try:
            return importlib.import_module("tools.vv.calculix_demo_gate.generate")
        except ModuleNotFoundError as error:
            if error.name not in ("numpy", "scipy"):
                raise
            self.skipTest(f"missing {error.name}")

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

    def test_double_frd_retains_convergence_precision_and_rejects_bad_records(self):
        # Native binary FRD records use 32-bit IDs and unaligned doubles.
        # Unlike E12.5 output, these distinguish the fine-grid temperature extrema.
        def fixture(ids=(1, 2), value=318.897712345678):
            header = lambda tag, count, fmt: f"{tag:<6}{count:30d}{fmt:38d}\n".encode()
            return (header("    2C", 2, 3)
                    + struct.pack("=i3d", 1, 0, 0, 0.0016)
                    + struct.pack("=i3d", 2, 0.001, 0, 0.0016)
                    + header("    3C", 0, 2)
                    + b"  100CL  101 1.000000000           2                     0    1           3\n"
                    + b" -4  NDTEMP      1    1\n -5  T           1    1    0    0\n"
                    + struct.pack("=id", ids[0], value)
                    + struct.pack("=id", ids[1], 306.536412345678)
                    + b" 9999\n")

        module = self.generator()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "double.frd"
            path.write_bytes(fixture())
            result = module.parse_results(path)
            self.assertAlmostEqual(result["temperature_blocks"][0][1],
                                   318.897712345678, delta=1e-12)
            for malformed in (fixture((1, 1)), fixture((1, 3)),
                              fixture(value=float("nan")), fixture()[:-10]):
                path.write_bytes(malformed)
                with self.assertRaises(ValueError):
                    module.parse_results(path)

    def test_full_and_mirrored_qois_use_identical_triangulation(self):
        module = self.generator()
        inputs = module.load_inputs()
        values = []
        with tempfile.TemporaryDirectory() as directory:
            for full in (False, True):
                mesh = module.build_mesh(inputs, "L1", full=full)
                nodes = mesh["nodes"]
                temperatures = {n: 310.0 for n in nodes}
                displacements = {}
                for n, xyz in nodes.items():
                    x, y = [(v - (0.01 if full else 0)) * 1000 for v in xyz[:2]]
                    w = 1e-7 * (x*x + y*y + 0.003*x*x*y*y)
                    displacements[n] = (0, 0, w + (1e-6 + 2e-8*x - 3e-8*y if full else 0))
                thermal = {"nodes": nodes, "temperature_blocks": [temperatures]}
                mechanical = dict(thermal, displacement_blocks=[displacements])
                values.append(module.compute_qoi(thermal, mechanical, mesh, inputs,
                                                 Path(directory), str(full)))
        for key in ("W_bfp_m", "W_bfp_nodes_m"):
            self.assertTrue(math.isclose(values[0][key], values[1][key],
                                         rel_tol=1e-12, abs_tol=0), key)

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

    def test_failed_solver_retains_measured_resource_evidence(self):
        module = self.generator()
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            fake = work / "failed_ccx"
            fake.write_text(f"#!{sys.executable}\nraise SystemExit(7)\n")
            fake.chmod(0o755)
            with patch.object(module, "CCX", str(fake)):
                with self.assertRaises(RuntimeError):
                    module.run_ccx(work, "failed")
            evidence = json.loads((work / "failed.failure.json").read_text())
            self.assertEqual(evidence["returncode"], 7)
            self.assertEqual(evidence["label"], "实测")
            self.assertGreater(evidence["peak_rss_KiB"], 0)
            self.assertGreater(evidence["wall_seconds"], 0)
            self.assertFalse((work / "REFERENCE.json").exists())

    def test_memory_limit_terminates_and_reaps_solver(self):
        module = self.generator()
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            fake = work / "sleeping_ccx"
            fake.write_text(f"#!{sys.executable}\nimport time\ntime.sleep(60)\n")
            fake.chmod(0o755)
            processes = []
            launch = subprocess.Popen

            def record_launch(*args, **kwargs):
                process = launch(*args, **kwargs)
                processes.append(process)
                return process

            with patch("subprocess.Popen", side_effect=record_launch), patch.object(
                module, "process_tree_rss", return_value=8 * 1024**2 + 1
            ):
                with self.assertRaisesRegex(ValueError, "8 GiB"):
                    module.measured_solver(work, "guard", str(fake))
            self.assertEqual(len(processes), 1)
            self.assertIsNotNone(processes[0].poll(), "memory guard must reap the solver")
            evidence = json.loads((work / "guard.failure.json").read_text())
            self.assertEqual(evidence["peak_rss_KiB"], 8 * 1024**2 + 1)
            self.assertFalse((work / "REFERENCE.json").exists())

    def test_assessment_rejects_zero_differences_and_preserves_status_precedence(self):
        module = self.generator()

        def level(w, t, top):
            return {"W_bfp_m": w, "T_max_K": t, "T_top_min_K": top,
                    "heat_source": {"relative_error": 0.0},
                    "temperature_transfer": {"max_abs_delta_K": 0.0}}

        levels = [level(1.004, 308.154, 303.154), level(1.001, 308.151, 303.151),
                  level(1.0, 308.15, 303.15)]
        symmetry = level(1.004, 308.154, 303.154)
        cross = level(1.0, 308.15, 303.15)
        qualified = module.assess_reference(levels, symmetry, cross, 298.15)
        self.assertEqual(qualified["status"], "合格")
        self.assertEqual(set(qualified["selected"].values()), {"richardson"})
        cross["W_bfp_m"] = 1.01
        self.assertEqual(module.assess_reference(levels, symmetry, cross, 298.15)["status"],
                         "需讨论")
        levels[2]["T_max_K"] = levels[1]["T_max_K"]
        rejected = module.assess_reference(levels, symmetry, cross, 298.15)
        self.assertEqual(rejected["status"], "参考不合格")
        self.assertIsNone(rejected["observed_order"]["T_max_K"])
        self.assertFalse(rejected["criteria"]["b"])

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
        path = ARTIFACT_DIR / "REFERENCE.json"
        self.assertTrue(path.is_file(), "missing required DEMO-GATE REFERENCE.json")
        artifact = json.loads(path.read_text(encoding="utf-8"))
        recorded_version = artifact["calculix_version"]
        module = self.generator()
        self.assertEqual(module.ccx_version(), recorded_version)
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
            points[:, :3] = np.round(points[:, :3], 9)
            points = points[np.lexsort((points[:, 0], points[:, 1]))]
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


def load_tests(loader, standard_tests, pattern):
    from tests.vv.calculix_demo_gate.test_reference_json import reference_acceptance_required

    if reference_acceptance_required(ARTIFACT_DIR):
        return standard_tests
    comparisons = {"test_l2_l3_deck_hashes_match_recorded_values_without_solver",
                   "test_l1_deck_bytes_and_recomputed_qois"}
    print("DEMO-GATE generator configuration: artifact comparisons deferred to V&V qualification.")
    return unittest.TestSuite(
        unittest.TestSuite(test for test in group if test._testMethodName not in comparisons)
        for group in standard_tests
    )


if __name__ == "__main__":
    if len(sys.argv) == 2 and Path(sys.argv[1]).is_dir():
        sys.argv.pop()
    result = unittest.main(verbosity=2, exit=False).result
    if not result.wasSuccessful():
        raise SystemExit(1)
    raise SystemExit(77 if result.skipped else 0)
