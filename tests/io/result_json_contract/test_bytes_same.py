# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
"""Repeated CLI writes preserve the frozen bytes and existing mode contract."""
from pathlib import Path
import sys

ROOT = Path(sys.argv.pop(1)).resolve()
for directory in (ROOT, ROOT / 'tests/io/result_json_export',
                  ROOT / 'tests/io/result_json_followup',
                  ROOT / 'tests/unit/result_summary'):
    sys.path.insert(0, str(directory))

import json
import stat
import unittest

from contract_fixtures import FollowupCase
from followup_fixtures import cli_umask


class BytesSame(FollowupCase):
    def assert_repeated_bytes_and_mode(self, target, masks, expected_mode):
        expected = (json.dumps(self.expected(), indent=2, sort_keys=False,
                               ensure_ascii=False, allow_nan=False) + '\n').encode('utf-8')
        outputs = []
        for index, mask in enumerate(masks):
            with self.subTest(write=index + 1, umask=oct(mask)):
                # The shared helper supplies only the required metadata CLI flags.
                reply = cli_umask(self.run, target, mask)
                self.assertEqual(reply.returncode, 0, reply.stderr)
                self.assertEqual(reply.stdout, b'')
                self.assertEqual(reply.stderr, b'')
                outputs.append(target.read_bytes())
                self.assertEqual(outputs[-1], expected)
                self.assertEqual(stat.S_IMODE(target.stat().st_mode), expected_mode)
        self.assertEqual(outputs, [expected, expected])

    def test_repeated_writes_keep_existing_mode_and_exact_bytes(self):
        self.target.write_text('pre-existing synthetic output\n', encoding='utf-8')
        self.target.chmod(0o640)
        self.assert_repeated_bytes_and_mode(self.target, (0o022, 0o077), 0o640)

    def test_repeated_fresh_writes_apply_child_umask_and_exact_bytes(self):
        for mask, mode in ((0o022, 0o644), (0o077, 0o600)):
            with self.subTest(umask=oct(mask)):
                target = self.base / f'fresh-{mask:o}.json'
                self.assertFalse(target.exists())
                self.assert_repeated_bytes_and_mode(target, (mask, mask), mode)


if __name__ == '__main__':
    unittest.main()
