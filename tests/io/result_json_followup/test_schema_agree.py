# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
"""Acceptance coverage for result.json followup behavior."""
from pathlib import Path
import sys

ROOT = Path(sys.argv.pop(1)).resolve()
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / 'tests/io/result_json_export'))
sys.path.insert(0, str(ROOT / 'tests/unit/result_summary'))

import unittest

try:
    from jsonschema import Draft202012Validator
except ImportError:
    print('SKIP: jsonschema unavailable; schema agreement requires Draft202012Validator')
    raise SystemExit(77)

from followup_fixtures import (FollowupCase, boundary_cases, candidate_for,
                               exporter_accepts, schema_for, shared)


class SchemaAgreement(FollowupCase):
    def test_schema_boundaries_agree_with_exporter(self):
        schema = schema_for(ROOT)
        Draft202012Validator.check_schema(schema)
        validator = Draft202012Validator(schema)
        base = self.expected(reference=dict(
            value=1.0, type=schema['properties']['reference']['properties']['type']['enum'][0],
            source='合成参考'))
        self.assertTrue(validator.is_valid(base))
        summary = shared.summarize(self.run)
        cases = list(boundary_cases(schema))
        self.assertGreaterEqual(len(cases), 40)
        self.assertEqual(len(cases), 67)
        print(f'schema agreement: {len(cases)} inputs', flush=True)
        for field, value, expected in cases:
            with self.subTest(field=field, value=value):
                candidate = candidate_for(base, field, value)
                accepted = exporter_accepts(self, summary, candidate)
                valid = validator.is_valid(candidate)
                self.assertEqual(accepted, valid,
                                 list(validator.iter_errors(candidate)))
                self.assertEqual(accepted, expected)
                if field == 'data_class' and value == 'real':
                    self.assertTrue(accepted)
                    self.assertTrue(valid)


if __name__ == '__main__':
    unittest.main()
