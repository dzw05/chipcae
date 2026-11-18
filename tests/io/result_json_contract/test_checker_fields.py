# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
"""Compare optional-field acceptance with the repository's result checker."""
from pathlib import Path
import sys

ROOT = Path(sys.argv.pop(1)).resolve()
for directory in (ROOT, ROOT / 'tests/io/result_json_export',
                  ROOT / 'tests/io/result_json_followup',
                  ROOT / 'tests/unit/result_summary'):
    sys.path.insert(0, str(directory))

import unittest

from contract_fixtures import (FollowupCase, boundary_cases, exporter_candidate,
                               schema_for, shared)
from tools.vv.check_result_data_class import check_file


class CheckerFields(FollowupCase):
    def test_optional_field_boundaries_agree_with_checker(self):
        cases = boundary_cases(schema_for(ROOT))
        print(f'checker fields: {len(cases)} inputs', flush=True)
        self.assertGreaterEqual(len(cases), 20)
        summary = shared.summarize(self.run)
        for case in cases:
            with self.subTest(case=case.name):
                accepted, candidate = exporter_candidate(self, summary, case)
                self.target.write_bytes(shared.encoded(candidate))
                reasons = check_file(self.target)
                self.assertEqual(accepted, case.valid, case.name)
                if case.options.get('data_class') == 'real':
                    self.assertTrue(accepted)
                    self.assertEqual(reasons, ['real result must stay local (D03)'])
                else:
                    self.assertEqual(accepted, reasons == [], reasons)


if __name__ == '__main__':
    unittest.main()
