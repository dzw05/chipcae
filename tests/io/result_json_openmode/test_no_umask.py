# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
"""Kernel-applied modes, exclusive temporary creation and descriptor ownership."""
from pathlib import Path
import sys

ROOT = Path(sys.argv.pop(1)).resolve()
for directory in (ROOT, ROOT / 'tests/io/result_json_export',
                  ROOT / 'tests/io/result_json_followup',
                  ROOT / 'tests/unit/result_summary'):
    sys.path.insert(0, str(directory))

import ast
import errno
import os
import stat
import unittest
from unittest.mock import patch

# Import the established fixture first, retaining dependency exit code 77.
from followup_fixtures import FollowupCase, cli_umask, exporter, shared


class NoUmask(FollowupCase):
    def test_writer_and_reachable_local_helpers_never_access_umask(self):
        module = ast.parse(Path(exporter.__file__).read_text(encoding='utf-8'))
        functions = {node.name: node for node in module.body
                     if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))}
        pending = ['write_result_json']
        visited = set()
        while pending:
            name = pending.pop()
            if name in visited:
                continue
            visited.add(name)
            for node in ast.walk(functions[name]):
                with self.subTest(function=name, line=getattr(node, 'lineno', None)):
                    if isinstance(node, ast.Attribute):
                        self.assertNotEqual(node.attr, 'umask',
                                            'writer must let the kernel apply umask')
                    if isinstance(node, ast.Call) and isinstance(node.func, ast.Name):
                        self.assertNotEqual(node.func.id, 'umask',
                                            'a direct umask call also changes process state')
                        if node.func.id in functions:
                            pending.append(node.func.id)

    def assert_write_without_umask(self, obj):
        original = os.umask(0o027)
        try:
            with patch.object(os, 'umask', side_effect=AssertionError(
                    'write_result_json must not call os.umask')) as forbidden:
                exporter.write_result_json(self.target, obj)
                self.assertEqual(self.target.read_bytes(), shared.encoded(obj))
                self.assertEqual(stat.S_IMODE(self.target.stat().st_mode), 0o640)
                self.assertEqual(list(self.base.glob('.result-json-*.tmp')), [])
                forbidden.assert_not_called()
            # Read the mask only after the patch has ended; finally restores it.
            self.assertEqual(os.umask(0o027), 0o027)
        finally:
            os.umask(original)

    def test_new_target_succeeds_without_reading_or_changing_umask(self):
        obj = self.expected(notes=['新建：内核应用 umask'])
        # New files must keep the mode created by the kernel without chmod.
        with patch.object(os, 'chmod', side_effect=AssertionError(
                'new targets must retain the kernel-created mode')):
            self.assert_write_without_umask(obj)

    def test_existing_target_succeeds_without_reading_or_changing_umask(self):
        self.target.write_bytes(b'old result\n')
        self.target.chmod(0o640)
        self.assert_write_without_umask(self.expected(notes=['替换：保留 0640']))

    def test_cli_creation_and_second_write_keep_bytes_and_child_umask_mode(self):
        expected_bytes = shared.encoded(self.expected())
        for mask, expected_mode in ((0o022, 0o644), (0o077, 0o600)):
            with self.subTest(umask=oct(mask)):
                target = self.base / f'result-{mask:o}.json'
                for write in (1, 2):
                    with self.subTest(write=write):
                        reply = cli_umask(self.run, target, mask)
                        self.assertEqual(reply.returncode, 0, reply.stderr)
                        self.assertEqual(reply.stdout, b'')
                        self.assertEqual(reply.stderr, b'')
                        self.assertEqual(target.read_bytes(), expected_bytes)
                        self.assertEqual(stat.S_IMODE(target.stat().st_mode), expected_mode)
                        self.assertEqual(list(self.base.glob('.result-json-*.tmp')), [])


class TemporaryOwnership(FollowupCase):
    def setUp(self):
        super().setUp()
        self.old_bytes = b'old result must survive\n'
        self.target.write_bytes(self.old_bytes)
        self.target.chmod(0o640)
        self.obj = self.expected(notes=['描述符所有权'])

    def assert_old_target_and_no_temporary(self):
        self.assertEqual(self.target.read_bytes(), self.old_bytes)
        self.assertEqual(stat.S_IMODE(self.target.stat().st_mode), 0o640)
        self.assertEqual(list(self.base.glob('.result-json-*.tmp')), [])

    def assert_error_contract(self, error):
        self.assertEqual(error.code, 14)
        self.assertEqual(error.name, 'INPUT_INVALID')

    def assert_descriptor_closed(self, fd):
        with self.assertRaises(OSError) as caught:
            os.fstat(fd)
        self.assertEqual(caught.exception.errno, errno.EBADF)

    @staticmethod
    def close_if_open(fd):
        # A descriptor leak should fail the assertion without leaking in the test.
        try:
            os.close(fd)
        except OSError as exc:
            if exc.errno != errno.EBADF:
                raise

    def record_open(self, descriptors):
        real_open = os.open

        def create(path, flags, mode=0o777):
            fd = real_open(path, flags, mode)
            descriptors.append(fd)
            self.addCleanup(self.close_if_open, fd)
            return fd

        return create

    def test_candidate_collision_keeps_unowned_file_and_retries_exclusive_creation(self):
        real_open = os.open
        attempts = []
        collision_bytes = b'another writer owns this candidate\n'

        def collide_once(path, flags, mode=0o777):
            candidate = Path(path)
            attempts.append((candidate, flags, mode))
            if len(attempts) == 1:
                fd = real_open(candidate, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
                try:
                    os.write(fd, collision_bytes)
                finally:
                    os.close(fd)
            # First call raises a genuine FileExistsError; the next uses the OS.
            return real_open(candidate, flags, mode)

        with patch.object(os, 'open', side_effect=collide_once):
            exporter.write_result_json(self.target, self.obj)
        self.assertGreaterEqual(len(attempts), 2, 'exclusive creation must retry a collision')
        collision = attempts[0][0]
        self.assertEqual(collision.read_bytes(), collision_bytes)
        self.assertEqual(self.target.read_bytes(), shared.encoded(self.obj))
        self.assertEqual(stat.S_IMODE(self.target.stat().st_mode), 0o640)
        self.assertEqual(list(self.base.glob('.result-json-*.tmp')), [collision])
        for candidate, flags, mode in attempts:
            with self.subTest(candidate=candidate.name):
                self.assertEqual(candidate.parent, self.target.parent)
                self.assertTrue(candidate.match('.result-json-*.tmp'))
                self.assertEqual(flags, os.O_WRONLY | os.O_CREAT | os.O_EXCL)
                self.assertEqual(mode, 0o666)

    def test_fdopen_failure_closes_created_descriptor_and_preserves_old_target(self):
        descriptors = []
        with patch.object(os, 'open', side_effect=self.record_open(descriptors)), \
                patch.object(os, 'fdopen', side_effect=OSError('controlled fdopen failure')):
            with self.assertRaises(exporter.ResultJsonError) as caught:
                exporter.write_result_json(self.target, self.obj)
        self.assert_error_contract(caught.exception)
        self.assertIn('controlled fdopen failure', str(caught.exception))
        self.assertEqual(len(descriptors), 1)
        self.assert_descriptor_closed(descriptors[0])
        self.assert_old_target_and_no_temporary()

    def test_write_failure_closes_stream_and_preserves_old_target(self):
        real_fdopen = os.fdopen
        streams = []
        descriptors = []

        def failing_stream(fd, *args, **kwargs):
            stream = real_fdopen(fd, *args, **kwargs)
            streams.append(stream)
            self.addCleanup(stream.close)
            failure = patch.object(stream, 'write', side_effect=OSError('controlled write failure'))
            failure.start()
            self.addCleanup(failure.stop)
            return stream

        with patch.object(os, 'open', side_effect=self.record_open(descriptors)), \
                patch.object(os, 'fdopen', side_effect=failing_stream):
            with self.assertRaises(exporter.ResultJsonError) as caught:
                exporter.write_result_json(self.target, self.obj)
        self.assert_error_contract(caught.exception)
        self.assertIn('controlled write failure', str(caught.exception))
        self.assertEqual(len(streams), 1)
        self.assertTrue(streams[0].closed)
        self.assertEqual(len(descriptors), 1)
        self.assert_descriptor_closed(descriptors[0])
        self.assert_old_target_and_no_temporary()

    def test_replace_failure_preserves_old_bytes_mode_and_cleans_closed_temporary(self):
        descriptors = []

        def deny_replace(temporary, target):
            self.assertEqual(Path(target), self.target)
            self.assertEqual(Path(temporary).read_bytes(), shared.encoded(self.obj))
            self.assertEqual(len(descriptors), 1)
            self.assert_descriptor_closed(descriptors[0])
            raise PermissionError('controlled replace failure')

        with patch.object(os, 'open', side_effect=self.record_open(descriptors)), \
                patch.object(os, 'replace', side_effect=deny_replace):
            with self.assertRaises(exporter.ResultJsonError) as caught:
                exporter.write_result_json(self.target, self.obj)
        self.assert_error_contract(caught.exception)
        self.assertIn('controlled replace failure', str(caught.exception))
        self.assert_old_target_and_no_temporary()

    def test_successful_replace_observes_a_closed_descriptor(self):
        real_replace = os.replace
        descriptors = []
        replaced = []

        def replace_closed(temporary, target):
            self.assertEqual(len(descriptors), 1)
            self.assert_descriptor_closed(descriptors[0])
            self.assertEqual(Path(temporary).parent, self.target.parent)
            self.assertEqual(Path(temporary).read_bytes(), shared.encoded(self.obj))
            self.assertEqual(self.target.read_bytes(), self.old_bytes)
            self.assertEqual(stat.S_IMODE(self.target.stat().st_mode), 0o640)
            replaced.append(Path(temporary))
            return real_replace(temporary, target)

        with patch.object(os, 'open', side_effect=self.record_open(descriptors)), \
                patch.object(os, 'replace', side_effect=replace_closed):
            exporter.write_result_json(self.target, self.obj)
        self.assertEqual(len(replaced), 1)
        self.assertFalse(replaced[0].exists())
        self.assertEqual(self.target.read_bytes(), shared.encoded(self.obj))
        self.assertEqual(stat.S_IMODE(self.target.stat().st_mode), 0o640)
        self.assertEqual(list(self.base.glob('.result-json-*.tmp')), [])


if __name__ == '__main__':
    unittest.main()
