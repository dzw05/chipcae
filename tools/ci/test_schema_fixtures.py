"""Tests for shared schema fixtures and stack examples."""

from __future__ import annotations

import json
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class SchemaFixturesTest(unittest.TestCase):
    def test_shared_fixture_validator(self):
        result = subprocess.run(
            [sys.executable, str(ROOT / "schema/tests/validate_fixtures.py"), str(ROOT)],
            cwd=ROOT,
            capture_output=True,
            text=True,
        )
        output = f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        dependency_output = output.lower()
        missing_dependency = result.returncode == 77 or (
            any(name in dependency_output for name in ("jsonschema", "referencing"))
            and any(
                marker in dependency_output
                for marker in ("no module named", "cannot import", "required", "missing")
            )
        )
        message = "Shared fixture validator failed"
        if missing_dependency:
            message += ": missing jsonschema/referencing dependency"
        self.assertEqual(result.returncode, 0, f"{message}\n{output}")
        self.assertFalse(missing_dependency, f"{message}\n{output}")

    def test_stack_examples(self):
        try:
            import jsonschema
        except ImportError as error:
            self.fail(f"missing jsonschema/referencing dependency: {error}")

        schema = json.loads((ROOT / "schema/stack-0.2.schema.json").read_text(encoding="utf-8"))
        validator = jsonschema.Draft202012Validator(schema)
        examples = sorted((ROOT / "schema/examples").glob("*.stack.json"))
        self.assertTrue(examples, "No schema/examples/*.stack.json examples found")
        for path in examples:
            with self.subTest(path=path):
                try:
                    validator.validate(json.loads(path.read_text(encoding="utf-8")))
                except (jsonschema.ValidationError, json.JSONDecodeError) as error:
                    self.fail(f"{path}: {error}")


if __name__ == "__main__":
    unittest.main()
