# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
"""Public docstrings must state the frozen result writer contract."""
from pathlib import Path
import sys

ROOT = Path(sys.argv.pop(1)).resolve()
for directory in (ROOT, ROOT / 'tests/io/result_json_export',
                  ROOT / 'tests/io/result_json_followup',
                  ROOT / 'tests/unit/result_summary'):
    sys.path.insert(0, str(directory))

import re
import unittest

from contract_fixtures import exporter


class Docs(unittest.TestCase):
    def test_writer_documents_atomic_replacement_and_permission_rules(self):
        doc = ' '.join((exporter.write_result_json.__doc__ or '').split())
        requirements = {
            'atomic replacement': r'(?i)atomic|原子',
            'closed temporary file': r'(?i)closed.*temporar|关闭.*临时',
            'existing target permissions': r'S_IMODE|保留.*权限',
            'new target mode formula': r'0o666\s*&\s*~\s*umask',
        }
        for requirement, pattern in requirements.items():
            with self.subTest(requirement=requirement):
                self.assertRegex(doc, pattern)

    def test_writer_documents_process_umask_race_and_symlink_behavior(self):
        doc = ' '.join((exporter.write_result_json.__doc__ or '').split())
        requirements = {
            'exclusive creation operation': r'os\.open',
            'kernel applies umask': r'(?i)kernel.{0,80}umask|umask.{0,80}kernel|内核.*umask',
            'process umask is not modified': (
                r'(?i)(?:does not|never|without)\s+(?:modify|change|modifying|changing)'
                r'.{0,40}process.{0,20}umask|不修改.*进程.*umask'
            ),
            'stat/replace race': r'TOCTOU|竞态',
            'stat follows symlinks': r'(?i)symlink|符号链接',
            'stat operation': r'os\.stat',
            'replace operation': r'os\.replace',
            'link following': r'(?i)follow|跟随',
            'replacement is regular': r'(?i)regular file|普通文件',
        }
        for requirement, pattern in requirements.items():
            with self.subTest(requirement=requirement):
                self.assertRegex(doc, pattern)

    def test_builder_documents_validation_local_date_and_frozen_order(self):
        doc = ' '.join((exporter.result_json_from_summary.__doc__ or '').split())
        requirements = {
            'finite validation': r'non-finite|非有限',
            'serialization': r'serialization|序列化',
            'local calendar day': r'本工具使用进程本地日历日',
            'stability guarantee': r'stability|稳定性',
            'frozen order': r'frozen|冻结',
            'byte comparison': r'byte|逐字节|按字节',
            'JSON member order has no semantics': r'unordered|no semantic|not.*semantic|无.*语义|不.*语义',
            'schema version requirement': r'schema.{0,60}(?:version|版本)|版本.{0,60}schema',
            'version must increase': r'increase|increment|bump|升|递增|提高',
        }
        for requirement, pattern in requirements.items():
            with self.subTest(requirement=requirement):
                self.assertRegex(doc, re.compile(pattern, re.IGNORECASE))


if __name__ == '__main__':
    unittest.main()
