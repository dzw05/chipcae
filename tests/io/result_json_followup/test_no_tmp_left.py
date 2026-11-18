# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
"""Acceptance coverage for result.json followup behavior."""
from pathlib import Path
import sys

ROOT = Path(sys.argv.pop(1)).resolve()
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / 'tests/io/result_json_export'))
sys.path.insert(0, str(ROOT / 'tests/unit/result_summary'))

import os
import stat
import unittest
from unittest.mock import patch

from followup_fixtures import FollowupCase, exporter, shared


class NoTemporaryResidue(FollowupCase):
    @unittest.skipIf(os.geteuid() == 0, 'root bypasses read-only directory permissions')
    def test_cli_read_only_destination_leaves_no_target_or_temporary(self):
        destination = self.base / 'read-only'
        destination.mkdir()
        target = destination / 'result.json'
        old_mode = stat.S_IMODE(destination.stat().st_mode)
        try:
            destination.chmod(0o555)
            reply = shared.cli(self.run, '-o', str(target))
            self.assertEqual(reply.returncode, 14, reply.stderr)
            self.assertIn(b'INPUT_INVALID', reply.stderr)
            self.assertFalse(target.exists())
            self.assertEqual(list(destination.glob('.result-json-*.tmp')), [])
        finally:
            destination.chmod(old_mode)

    def test_cli_missing_parent_stays_absent(self):
        destination = self.base / 'missing'
        reply = shared.cli(self.run, '-o', str(destination / 'result.json'))
        self.assertEqual(reply.returncode, 14, reply.stderr)
        self.assertIn(b'INPUT_INVALID', reply.stderr)
        self.assertFalse(destination.exists())
        self.assertEqual(list(self.base.rglob('.result-json-*.tmp')), [])

    def test_chmod_failure_preserves_old_file_and_cleans_closed_temporary(self):
        old_bytes = b'old result must survive\n'
        self.target.write_bytes(old_bytes)
        self.target.chmod(0o640)

        def deny_chmod(path, mode):
            # Inject only the OS failure after real temporary creation and writing.
            temporary = Path(path)
            self.assertEqual(temporary.parent, self.target.parent)
            self.assertTrue(temporary.name.startswith('.result-json-'))
            self.assertEqual(temporary.suffix, '.tmp')
            self.assertEqual(temporary.read_bytes(), shared.encoded(obj))
            raise PermissionError('controlled chmod failure')

        obj = self.expected()
        with patch.object(exporter.os, 'chmod', side_effect=deny_chmod):
            with self.assertRaises(exporter.ResultJsonError) as caught:
                exporter.write_result_json(self.target, obj)
        self.assertEqual(caught.exception.code, 14)
        self.assertEqual(caught.exception.name, 'INPUT_INVALID')
        self.assertEqual(self.target.read_bytes(), old_bytes)
        self.assertEqual(stat.S_IMODE(self.target.stat().st_mode), 0o640)
        self.assertEqual(list(self.base.glob('.result-json-*.tmp')), [])


if __name__ == '__main__':
    program = unittest.main(exit=False)
    if not program.result.wasSuccessful():
        raise SystemExit(1)
    if os.geteuid() == 0:
        print('SKIP: root bypasses read-only permissions; other cleanup checks ran')
        raise SystemExit(77)
