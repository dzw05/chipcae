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

from tools.vv.check_result_data_class import check_file
from followup_fixtures import (FollowupCase, boundary_cases, candidate_for,
                               exporter_accepts, schema_for, shared)


class CheckerAgreement(FollowupCase):
    def test_schema_boundaries_agree_with_exporter_and_checker(self):
        schema = schema_for(ROOT)
        base = self.expected(reference=dict(
            value=1.0, type=schema['properties']['reference']['properties']['type']['enum'][0],
            source='合成参考'))
        summary = shared.summarize(self.run)
        cases = list(boundary_cases(schema))
        self.assertGreaterEqual(len(cases), 40)
        self.assertEqual(len(cases), 67)
        print(f'checker agreement: {len(cases)} inputs', flush=True)
        for field, value, expected in cases:
            with self.subTest(field=field, value=value):
                candidate = candidate_for(base, field, value)
                accepted = exporter_accepts(self, summary, candidate)
                # Even rejected exporter inputs independently reach the checker.
                self.target.write_bytes(shared.encoded(candidate))
                reasons = check_file(self.target)
                if field == 'data_class' and value == 'real':
                    self.assertTrue(accepted)
                    self.assertEqual(reasons, ['real result must stay local (D03)'])
                else:
                    self.assertEqual(accepted, not reasons, reasons)
                self.assertEqual(accepted, expected)


if __name__ == '__main__':
    unittest.main()
