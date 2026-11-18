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

from followup_fixtures import exporter


class DocstringContract(unittest.TestCase):
    def test_builder_documents_validation_serialization_and_local_date(self):
        doc = exporter.result_json_from_summary.__doc__ or ''
        with self.subTest(requirement='recursive finite validation'):
            self.assertIn('non-finite', doc)
        with self.subTest(requirement='serialization convention'):
            self.assertTrue('serialization' in doc or '序列化' in doc, doc)
        with self.subTest(requirement='CLI local calendar default'):
            self.assertIn('本工具使用进程本地日历日', doc)


if __name__ == '__main__':
    unittest.main()
