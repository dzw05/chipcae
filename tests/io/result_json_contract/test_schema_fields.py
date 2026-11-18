# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
"""Compare emitted optional fields with the authoritative 0.2 schema."""
from pathlib import Path
import sys

ROOT = Path(sys.argv.pop(1)).resolve()
for directory in (ROOT, ROOT / 'tests/io/result_json_export',
                  ROOT / 'tests/io/result_json_followup',
                  ROOT / 'tests/unit/result_summary'):
    sys.path.insert(0, str(directory))

import json
import unittest

try:
    from jsonschema import Draft202012Validator
except ImportError:
    print('SKIP: jsonschema unavailable; schema fields require Draft202012Validator')
    raise SystemExit(77)

from contract_fixtures import (FollowupCase, boundary_cases, exporter_candidate,
                               schema_for, shared)


class SchemaFields(FollowupCase):
    def test_optional_field_boundaries_agree_with_schema(self):
        schema = schema_for(ROOT)
        Draft202012Validator.check_schema(schema)
        validator = Draft202012Validator(schema)
        cases = boundary_cases(schema)
        print(f'schema fields: {len(cases)} inputs', flush=True)
        self.assertGreaterEqual(len(cases), 20)
        summary = shared.summarize(self.run)
        for case in cases:
            with self.subTest(case=case.name):
                accepted, candidate = exporter_candidate(self, summary, case)
                # Round-trip the emitted object, including omission normalization.
                candidate = json.loads(shared.encoded(candidate))
                valid = validator.is_valid(candidate)
                self.assertEqual(accepted, case.valid, case.name)
                self.assertEqual(accepted, valid, list(validator.iter_errors(candidate)))
                if case.options.get('data_class') == 'real':
                    self.assertTrue(accepted)
                    self.assertTrue(valid)


if __name__ == '__main__':
    unittest.main()
