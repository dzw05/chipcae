# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
"""Acceptance coverage for result.json followup behavior."""
from pathlib import Path
import sys

ROOT = Path(sys.argv.pop(1)).resolve()
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / 'tests/io/result_json_export'))
sys.path.insert(0, str(ROOT / 'tests/unit/result_summary'))

import stat
import unittest

from followup_fixtures import FollowupCase, cli_umask


class ModeNew(FollowupCase):
    def test_cli_new_file_respects_child_umask(self):
        for mask, expected in ((0o022, 0o644), (0o077, 0o600)):
            with self.subTest(umask=oct(mask)):
                target = self.base / f'result-{mask:o}.json'
                reply = cli_umask(self.run, target, mask)
                self.assertEqual(reply.returncode, 0, reply.stderr)
                actual = stat.S_IMODE(target.stat().st_mode)
                self.assertEqual(actual, expected,
                                 f'umask {mask:03o}: expected {expected:04o}, got {actual:04o}')


if __name__ == '__main__':
    unittest.main()
