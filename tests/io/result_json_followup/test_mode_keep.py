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


def current_umask():
    old = os.umask(0)
    try:
        return old
    finally:
        os.umask(old)


class ModeKeep(FollowupCase):
    def test_replacement_keeps_user_mode_and_writes_second_object(self):
        first = self.expected()
        second = self.expected(notes=['第二次写出'])
        self.assertNotEqual(shared.encoded(first), shared.encoded(second))
        exporter.write_result_json(self.target, first)
        self.target.chmod(0o640)
        exporter.write_result_json(self.target, second)
        self.assertEqual(self.target.read_bytes(), shared.encoded(second))
        self.assertEqual(stat.S_IMODE(self.target.stat().st_mode), 0o640)

    def test_parent_umask_survives_success_and_write_failure(self):
        original = os.umask(0o027)
        try:
            exporter.write_result_json(self.target, self.expected())
            self.assertEqual(current_umask(), 0o027)
            failing = self.base / 'failed.json'
            with patch.object(exporter.os, 'replace', side_effect=PermissionError('replace denied')):
                with self.assertRaises(exporter.ResultJsonError) as caught:
                    exporter.write_result_json(failing, self.expected())
            self.assertEqual(caught.exception.code, 14)
            self.assertEqual(caught.exception.name, 'INPUT_INVALID')
            self.assertEqual(current_umask(), 0o027)
            self.assertFalse(failing.exists())
            self.assertEqual(list(self.base.glob('.result-json-*.tmp')), [])
        finally:
            os.umask(original)


if __name__ == '__main__':
    unittest.main()
