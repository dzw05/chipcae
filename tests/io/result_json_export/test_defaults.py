# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
import contextlib
from datetime import datetime
import json
import os
from pathlib import Path
import subprocess
import sys
import unittest
from unittest.mock import patch

from export_fixtures import ExportCase, cli, exporter, protocol_cli


@contextlib.contextmanager
def git_replies(replies):
    """Control external Git only; keep summary reading and JSON writing real.

    Support both common subprocess APIs without prescribing a private helper.
    None means Git failed, including stderr that the exporter must capture.
    """
    pending = iter(replies)
    calls = []

    def run(command, **kwargs):
        if not command or command[0] != 'git':
            raise AssertionError(f'unexpected external command: {command!r}')
        calls.append(([str(part) for part in command], kwargs))
        try:
            reply = next(pending)
        except StopIteration:
            raise AssertionError('unexpected additional Git query') from None
        text = kwargs.get('text') or kwargs.get('universal_newlines') or kwargs.get('encoding')
        out = '' if reply is None else reply + '\n'
        err = 'fatal: controlled Git failure\n' if reply is None else ''
        if err and not (kwargs.get('capture_output') or
                        kwargs.get('stderr') in (subprocess.PIPE, subprocess.DEVNULL)):
            sys.stderr.write(err)
        if not text:
            out, err = out.encode(), err.encode()
        result = subprocess.CompletedProcess(command, 128 if reply is None else 0, out, err)
        if kwargs.get('check') and result.returncode:
            raise subprocess.CalledProcessError(result.returncode, command, out, err)
        return result

    def check_output(command, **kwargs):
        return run(command, check=True, **kwargs).stdout

    with patch.dict(os.environ, {}, clear=False):
        os.environ.pop('CHIPCAE_GIT_SHA', None)
        with patch('subprocess.run', side_effect=run), \
                patch('subprocess.check_output', side_effect=check_output):
            yield calls


class Defaults(ExportCase):
    def test_environment_commit_and_process_local_date(self):
        before = datetime.now().strftime('%Y%m%d')
        completed = cli(self.run, env={'CHIPCAE_GIT_SHA': 'abcdef1'},
                        date=None, commit=None)
        after = datetime.now().strftime('%Y%m%d')
        self.assertEqual(completed.returncode, 0, completed.stderr.decode())
        self.assertEqual(completed.stderr, b'')
        obj = json.loads(completed.stdout)
        self.assertEqual(obj['commit'], 'abcdef1')
        self.assertRegex(obj['date'], r'^[0-9]{8}$')
        # Both bounds are local dates; tolerate only a midnight crossed in flight.
        self.assertIn(obj['date'], (before, after))
        self.assertNotIn('notes', obj)

    def test_mocked_local_calendar_date_is_used_exactly(self):
        class LocalDateTime(datetime):
            @classmethod
            def now(cls):
                return cls(2031, 2, 3, 0, 4, 5)

        with patch.object(exporter, 'datetime', LocalDateTime):
            code, out, err = protocol_cli(self.run, date=None)
        self.assertEqual(code, 0, err)
        self.assertEqual(err, '')
        self.assertEqual(json.loads(out)['date'], '20310203')

    def test_two_git_failures_append_placeholder_note_after_user_notes(self):
        with git_replies([None, None]) as calls:
            code, out, err = protocol_cli(self.run, '--note', 'a', '--note', 'a',
                                          commit=None)
        self.assertEqual(code, 0, err)
        self.assertEqual(err, '')
        obj = json.loads(out)
        self.assertEqual(obj['commit'], '0000000')
        self.assertEqual(obj['notes'], ['a', 'a', 'commit defaulted to 0000000'])
        self.assertEqual(len(calls), 2)

    def test_git_filesystem_root_query_wins_before_cwd(self):
        with git_replies(['123abcd']) as calls:
            code, out, err = protocol_cli(self.run, commit=None)
        self.assertEqual(code, 0, err)
        self.assertEqual(err, '')
        self.assertEqual(json.loads(out)['commit'], '123abcd')
        self.assertNotIn('notes', json.loads(out))
        self.assertEqual(len(calls), 1)
        self.assertEqual(calls[0][0], [
            'git', '-C', self.run.resolve().anchor, 'rev-parse', 'HEAD'])

    def test_git_cwd_fallback_after_root_query_failure(self):
        with git_replies([None, '7654321']) as calls:
            code, out, err = protocol_cli(self.run, commit=None)
        self.assertEqual(code, 0, err)
        self.assertEqual(err, '')
        self.assertEqual(json.loads(out)['commit'], '7654321')
        self.assertEqual([command for command, _ in calls], [
            ['git', '-C', self.run.resolve().anchor, 'rev-parse', 'HEAD'],
            ['git', 'rev-parse', 'HEAD']])
        self.assertEqual(Path(calls[1][1].get('cwd', Path.cwd())), Path.cwd())

    def test_explicit_and_environment_commit_precede_git(self):
        for explicit, expected in [('7654321', '7654321'), (None, 'abcdef1')]:
            with self.subTest(explicit=explicit):
                with git_replies([]) as calls:
                    os.environ['CHIPCAE_GIT_SHA'] = 'abcdef1'
                    code, out, err = protocol_cli(self.run, commit=explicit)
                self.assertEqual(code, 0, err)
                self.assertEqual(err, '')
                self.assertEqual(json.loads(out)['commit'], expected)
                self.assertEqual(calls, [])

    def test_invalid_explicit_or_environment_commit_is_not_replaced(self):
        for explicit, environment in [('', 'abcdef1'), ('ABCDEF1', 'abcdef1'),
                                       (None, ''), (None, 'not-a-sha')]:
            with self.subTest(explicit=explicit, environment=environment):
                with git_replies([]):
                    os.environ['CHIPCAE_GIT_SHA'] = environment
                    result = protocol_cli(self.run, commit=explicit)
                self.assert_failure(result, 'commit')


if __name__ == '__main__':
    unittest.main(argv=[__file__])
