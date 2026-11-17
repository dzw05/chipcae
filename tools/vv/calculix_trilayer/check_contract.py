#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
"""Manual regressions for the shared schema-consumer API and CLI."""
import copy
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import jsonschema

from contract import validate_reference


class ContractTest(unittest.TestCase):
    def test_consumer_entry_points(self):
        directory = Path(__file__).resolve().parent
        original = json.loads((directory / "REFERENCE.json").read_text(encoding="utf-8"))
        matching = copy.deepcopy(original)
        matching["W_bfp_m"] *= 1.01
        for mesh in matching["meshes"]:
            if mesh["name"] == matching["selected_mesh"]:
                mesh["W_bfp_m"] = matching["W_bfp_m"]
        mismatching = copy.deepcopy(matching)
        mismatching["W_bfp_m"] *= 1.01
        malformed = copy.deepcopy(original)
        del malformed["selected_mesh"]
        with tempfile.TemporaryDirectory() as temporary:
            artifact = Path(temporary) / "reference.json"
            for data, valid in ((original, True), (matching, True),
                                (mismatching, False), (malformed, False)):
                with self.subTest(valid=valid, data=data):
                    if valid:
                        validate_reference(data)
                    else:
                        with self.assertRaises(jsonschema.ValidationError):
                            validate_reference(data)
                    artifact.write_text(json.dumps(data), encoding="utf-8")
                    result = subprocess.run(
                        [sys.executable, str(directory / "contract.py"), str(artifact)],
                        capture_output=True, text=True, check=False,
                    )
                    self.assertEqual(result.returncode, 0 if valid else 1, result.stderr)
                    if not valid:
                        self.assertIn("Invalid reference:", result.stderr)


if __name__ == "__main__":
    unittest.main()
