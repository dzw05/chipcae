# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
"""Acceptance coverage for result.json followup behavior."""
from pathlib import Path
import sys

ROOT = Path(sys.argv.pop(1)).resolve()
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / 'tests/io/result_json_export'))
sys.path.insert(0, str(ROOT / 'tests/unit/result_summary'))

import json
import unittest

from followup_fixtures import FollowupCase, shared


class BytesSame(FollowupCase):
    def test_cli_bytes_keep_field_order_unicode_and_optional_metadata(self):
        expected = self.expected(
            reference=dict(value=1.25, type='解析', source='合成解析参考'),
            error=dict(value=0.125, metric='相对误差'),
            tolerance=dict(value=0.25, metric='容差'),
            notes=['合成测试', '第二条说明'], src_dirty=True)
        reply = shared.cli(
            self.run, '-o', str(self.target),
            '--reference-value', '1.25', '--reference-type', '解析',
            '--reference-source', '合成解析参考',
            '--error-value', '0.125', '--error-metric', '相对误差',
            '--tolerance-value', '0.25', '--tolerance-metric', '容差',
            '--note', '合成测试', '--note', '第二条说明', '--src-dirty')
        self.assertEqual(reply.returncode, 0, reply.stderr)
        serialized = json.dumps(expected, indent=2, sort_keys=False,
                                ensure_ascii=False, allow_nan=False) + '\n'
        self.assertEqual(self.target.read_bytes(), serialized.encode('utf-8'))


if __name__ == '__main__':
    unittest.main()
