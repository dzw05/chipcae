#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Unpublished references skip acceptance; present invalid artifacts still fail."""

import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch


HERE = Path(__file__).resolve().parent


def load_test_module(name):
    spec = importlib.util.spec_from_file_location(name, HERE / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class ArtifactAvailabilityTest(unittest.TestCase):
    def setUp(self):
        self.reference = load_test_module("test_reference_json")
        self.regen = load_test_module("test_regen_l1")

    def test_absent_artifact_skips_only_artifact_acceptance(self):
        with tempfile.TemporaryDirectory() as directory:
            for module, suite in (
                (self.reference, unittest.defaultTestLoader.loadTestsFromTestCase(
                    self.reference.DemoGateReferenceTest)),
                (self.regen, unittest.TestSuite([self.regen.DemoGateRegenTest(
                    "test_l1_deck_bytes_and_recomputed_qois")])),
            ):
                with self.subTest(module=module.__name__), patch.object(
                    module, "ARTIFACT_DIR", Path(directory)
                ):
                    result = unittest.TestResult()
                    suite.run(result)
                    self.assertTrue(result.wasSuccessful(), result.errors + result.failures)
                    self.assertGreater(result.testsRun, 0)
                    self.assertEqual(len(result.skipped), result.testsRun)

    def test_present_invalid_artifact_is_not_skipped(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "REFERENCE.json"
            for invalid in ("directory", "malformed JSON"):
                if invalid == "directory":
                    path.mkdir()
                    expected = AssertionError
                else:
                    path.rmdir()
                    path.write_text("{", encoding="utf-8")
                    expected = json.JSONDecodeError
                for module, check in (
                    (self.reference, self.reference.DemoGateReferenceTest().load_artifact),
                    (self.regen, self.regen.DemoGateRegenTest().test_l1_deck_bytes_and_recomputed_qois),
                ):
                    with self.subTest(invalid=invalid, module=module.__name__), patch.object(
                        module, "ARTIFACT_DIR", Path(directory)
                    ):
                        with self.assertRaises(expected):
                            check()


if __name__ == "__main__":
    if len(sys.argv) == 2 and Path(sys.argv[1]).is_dir():
        sys.argv.pop()
    unittest.main(verbosity=2)
