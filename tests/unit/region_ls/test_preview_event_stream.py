"""Acceptance for a strict file channel isolated from launcher stdout fragments."""

import json
import os
from pathlib import Path
import resource
import shlex
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

from preview_event_stream import (
    EventStreamError, check_serial_stdout, read_events, stdout_mixed,
)


def diagnostics(result):
    return (f'\ncommand: {shlex.join(result.args)}\nexit: {result.returncode}'
            f'\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}')


class EventReader(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='preview-events-reader-')
        self.addCleanup(temporary.cleanup)
        self.path = Path(temporary.name) / 'events'

    def test_clean_objects_and_ordered_pairs(self):
        contents = ('{"stage":"load","progress":0,"message":"载入"}\n'
                    '{"stage":"success","progress":1,"message":"done",'
                    '"nested":{"b":2,"a":1}}\n')
        self.path.write_bytes(contents.encode('utf-8'))
        self.assertEqual(read_events(self.path),
                         [json.loads(line) for line in contents[:-1].split('\n')])
        self.assertEqual(read_events(self.path, object_pairs_hook=list), [
            [('stage', 'load'), ('progress', 0), ('message', '载入')],
            [('stage', 'success'), ('progress', 1), ('message', 'done'),
             ('nested', [('b', 2), ('a', 1)])],
        ])
        check_serial_stdout(contents, self.path)
        self.assertIs(stdout_mixed(contents, self.path), False)

    def test_rejects_fragments_truncation_blanks_and_nonobjects(self):
        samples = [
            'run_config levels=1 n0=4x4 part={"stage":"mesh","progress":0.3,'
            '"message":"Building tensor grid"}\n',
            '$: cannot open instance or schema{"stage":"load","progress":0,'
            '"message":"Loading stack 0.2"}\n',
            '{"stage":"mesh","progress":0.3,"message":"Buil\n',
            '{"stage":"mesh"} trailing\n',
            '{"stage":"load"}\n\n{"stage":"success"}\n',
            '{"stage":"load"}\n\n',
            '', '{"stage":"load"}', '[1,2]\n', 'warning: x\n',
            ' \t\n', '{"stage":"load"}\n \t\n{"stage":"success"}\n',
            '{"stage":"mesh"} {"stage":"success"}\n',
            'null\n', '42\n', '"text"\n',
        ]
        for contents in samples:
            for hook in (None, list):
                with self.subTest(contents=contents, hook=hook):
                    self.path.write_bytes(contents.encode('utf-8'))
                    with self.assertRaises(EventStreamError) as caught:
                        read_events(self.path, object_pairs_hook=hook)
                    self.assertIn(str(self.path), str(caught.exception))
                    self.assertIn(contents, str(caught.exception))

    def test_invalid_utf8_reports_all_bytes(self):
        raw = b'{"message":"\xff"}\n'
        self.path.write_bytes(raw)
        with self.assertRaises(EventStreamError) as caught:
            read_events(self.path)
        self.assertIn(str(self.path), str(caught.exception))
        self.assertIn(repr(raw), str(caught.exception))

    def test_stdout_comparison_uses_exact_utf8_bytes(self):
        contents = '{"message":"网格"}\n'
        self.path.write_bytes(contents.encode('utf-8'))
        for changed in (contents[:-1], contents.replace('\n', '\r\n'),
                        'run_config levels=1 n0=4x4 part=' + contents):
            with self.subTest(stdout=changed):
                with self.assertRaises(EventStreamError):
                    check_serial_stdout(changed, self.path)
                self.assertIs(stdout_mixed(changed, self.path), True)


class PreviewEventStream(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        binary = os.environ.get('CHIPCAE_BINARY_DIR')
        if not binary:
            raise unittest.SkipTest('end-to-end cases require CHIPCAE_BINARY_DIR')
        cls.build = Path(binary)
        cls.cache = {}
        for line in (cls.build / 'CMakeCache.txt').read_text(encoding='utf-8').splitlines():
            if line and not line.startswith(('#', '//')) and '=' in line and ':' in line:
                key, value = line.split('=', 1)
                cls.cache[key.split(':', 1)[0]] = value
        for feature in ('CHIPCAE_ENABLE_JSONCONS', 'CHIPCAE_ENABLE_HDF5'):
            if cls.cache.get(feature) != 'ON':
                raise unittest.SkipTest(f'preview requires {feature}=ON')
        cls.preview = cls.build / 'preview' / 'chipcae-preview'
        if not cls.preview.is_file():
            raise AssertionError(f'enabled preview executable is missing: {cls.preview}')
        temporary = tempfile.TemporaryDirectory(prefix='preview-event-stream-')
        cls.addClassCleanup(temporary.cleanup)
        cls.root = Path(temporary.name)
        cls.fixture = Path(__file__).resolve().parents[3] / 'schema/examples/fcbga_demo.stack.json'
        cls.env = dict(os.environ, OMPI_ALLOW_RUN_AS_ROOT='1',
                       OMPI_ALLOW_RUN_AS_ROOT_CONFIRM='1')
        cls.reference = None

    def command(self, *flags):
        return [str(self.preview), str(self.fixture), '--nx', '2', '--ny', '2',
                '--levels', '0', '--max-ratio', '10', '-o', str(self.root / 'grid'), *flags]

    def invoke(self, command):
        # Keep bytes intact: text-mode newline normalization would mask CRLF drift.
        result = subprocess.run(command, capture_output=True, env=self.env, cwd=self.root, timeout=8)
        return subprocess.CompletedProcess(result.args, result.returncode,
                                           result.stdout.decode('utf-8'),
                                           result.stderr.decode('utf-8'))

    def events(self, result, path, launcher=False):
        try:
            events = read_events(path)
            if launcher:
                if stdout_mixed(result.stdout, path):
                    print(f'PREVIEW_STDOUT_MIXED: {self.id()}', file=sys.stderr)
            else:
                check_serial_stdout(result.stdout, path)
        except EventStreamError as error:
            self.fail(f'invalid events-file JSON: {error}{diagnostics(result)}')
        return events

    def success(self, result, path, launcher=False):
        self.assertEqual(result.returncode, 0, diagnostics(result))
        events = self.events(result, path, launcher)
        self.assertEqual(events[-1]['stage'], 'success', diagnostics(result))
        found = [event for event in events if event.get('stage') == 'success']
        self.assertEqual(len(found), 1, diagnostics(result))
        return found[0]

    def serial_reference(self):
        cls = type(self)
        if cls.reference is None:
            path = self.root / 'serial.events'
            result = self.invoke(self.command('--events-file', str(path)))
            summary = self.success(result, path)
            cls.reference = (result, path.read_bytes(), summary)
        return cls.reference

    def test_serial_file_equals_stdout(self):
        result, raw, _ = self.serial_reference()
        self.assertEqual(raw, result.stdout.encode('utf-8'), diagnostics(result))

    def test_stdout_unchanged_without_option(self):
        plain = self.invoke(self.command())
        self.assertEqual(plain.returncode, 0, diagnostics(plain))
        path = self.root / 'unchanged.events'
        enabled = self.invoke(self.command('--events-file', str(path)))
        context = diagnostics(plain) + diagnostics(enabled)
        self.assertEqual(enabled.returncode, plain.returncode, context)
        self.assertEqual(enabled.stdout.encode('utf-8'), plain.stdout.encode('utf-8'), context)
        self.assertEqual(enabled.stderr.encode('utf-8'), plain.stderr.encode('utf-8'), context)
        self.success(enabled, path)

    def test_mixed_stdout_launcher(self):
        # Flush before spawning so the fragment reliably adjoins the first event.
        launcher = ('import subprocess,sys; '
                    'sys.stdout.write("run_config levels=1 n0=4x4 part="); '
                    'sys.stdout.flush(); '
                    'sys.exit(subprocess.call(sys.argv[1:], stderr=subprocess.STDOUT))')
        path = self.root / 'mixed.events'
        result = self.invoke([sys.executable, '-c', launcher,
                              *self.command('--events-file', str(path))])
        mixed = self.root / 'mixed.stdout'
        mixed.write_bytes(result.stdout.encode('utf-8'))
        with self.assertRaises(EventStreamError):
            read_events(mixed)
        self.success(result, path, launcher=True)
        self.assertIs(stdout_mixed(result.stdout, path), True, diagnostics(result))
        _, serial_bytes, _ = self.serial_reference()
        self.assertEqual(path.read_bytes(), serial_bytes, diagnostics(result))

    def test_mpi_events_file(self):
        mpi = shutil.which('mpiexec')
        configured = self.cache.get('MPIEXEC_EXECUTABLE', '')
        if (self.cache.get('CHIPCAE_ENABLE_MPI') != 'ON' or not mpi or not configured
                or Path(mpi).resolve() != Path(configured).resolve()):
            self.skipTest('MPI-enabled preview and build-matching mpiexec required')
        path = self.root / 'mpi.events'
        command = [mpi, self.cache.get('MPIEXEC_NUMPROC_FLAG', '-n'), '2',
                   *self.command('--events-file', str(path))]
        result = self.invoke(command)
        summary = self.success(result, path, launcher=True)
        _, _, serial_summary = self.serial_reference()
        self.assertEqual(summary['mesh_sha256'], serial_summary['mesh_sha256'],
                         diagnostics(result))

    def error(self, result, message, prefix=False):
        context = diagnostics(result)
        self.assertEqual(result.returncode, 1, context)
        self.assertTrue(result.stdout.endswith('\n'), context)
        try:
            events = [json.loads(line) for line in result.stdout[:-1].split('\n')]
        except json.JSONDecodeError as error:
            self.fail(f'invalid stdout JSON for unopened events file: {error}{context}')
        self.assertTrue(all(isinstance(event, dict) for event in events), context)
        errors = [event for event in events if event.get('stage') == 'error']
        self.assertEqual(len(errors), 1, context)
        self.assertFalse(any(event.get('stage') == 'success' for event in events), context)
        if prefix:
            self.assertTrue(errors[0]['message'].startswith(message), context)
        else:
            self.assertEqual(errors[0]['message'], message, context)

    def test_events_file_errors(self):
        with self.subTest(case='missing directory'):
            path = self.root / 'absent' / 'events'
            result = self.invoke(self.command('--events-file', str(path)))
            self.error(result, 'events file: ', prefix=True)
            self.assertFalse(path.exists(), diagnostics(result))
        for suffix in ('grid', 'surface'):
            real = self.root / f'collision-{suffix}'
            real.mkdir()
            alias = self.root / f'alias-{suffix}'
            alias.symlink_to(real, target_is_directory=True)
            for aliased in (False, True):
                with self.subTest(case=f'{suffix} collision', alias=aliased):
                    target = real / 'output.vtkhdf'
                    event_path = (alias if aliased else real) / target.name
                    flags = ['-o', str(target)] if suffix == 'grid' else ['--surface', str(target)]
                    # Use one -o option: duplicate output flags have their own precedence.
                    command = self.command()
                    if suffix == 'grid':
                        command[command.index('-o') + 1] = str(target)
                        flags = []
                    result = self.invoke([*command, *flags, '--events-file', str(event_path)])
                    self.error(result, 'events file must differ from grid and surface paths')
                    self.events(result, event_path)
        with self.subTest(case='derived grid filename'):
            stem = self.root / 'derived'
            command = self.command()
            command[command.index('-o') + 1] = str(stem)
            path = Path(str(stem) + '.vtkhdf')
            result = self.invoke([*command, '--events-file', str(path)])
            self.error(result, 'events file must differ from grid and surface paths')
            self.events(result, path)

    def test_duplicate_option_does_not_open_or_truncate(self):
        for existing in (False, True):
            with self.subTest(existing=existing):
                first = self.root / f'duplicate-{existing}.events'
                second = self.root / f'duplicate-second-{existing}.events'
                if existing:
                    first.write_bytes(b'keep first\n')
                    second.write_bytes(b'keep second\n')
                result = self.invoke(self.command('--events-file', str(first),
                                                  '--events-file', str(second)))
                self.error(result, 'duplicate option: --events-file')
                if existing:
                    self.assertEqual(first.read_bytes(), b'keep first\n', diagnostics(result))
                    self.assertEqual(second.read_bytes(), b'keep second\n', diagnostics(result))
                else:
                    self.assertFalse(first.exists(), diagnostics(result))
                    self.assertFalse(second.exists(), diagnostics(result))

    def test_missing_or_empty_value_does_not_open_file(self):
        for flags in (['--events-file'], ['--events-file', '']):
            with self.subTest(flags=flags):
                before = set(self.root.iterdir())
                result = self.invoke(self.command(*flags))
                self.error(result, 'missing value for --events-file')
                self.assertEqual(set(self.root.iterdir()), before, diagnostics(result))

    def test_other_argument_error_is_mirrored(self):
        path = self.root / 'argument-error.events'
        command = [str(self.preview), str(self.fixture), '-o', str(self.root / 'bad-grid'),
                   '--events-file', str(path), '--nx']
        result = self.invoke(command)
        self.error(result, 'missing value for --nx')
        self.assertEqual(self.events(result, path)[-1]['stage'], 'error', diagnostics(result))
        self.assertEqual(path.read_bytes(), result.stdout.encode('utf-8'), diagnostics(result))

    def test_option_name_consumed_as_another_value_is_not_prescanned(self):
        command = self.command('--surface', '--events-file')
        result = self.invoke(command)
        # --surface consumes this token as a path; this is not a missing event-file value.
        self.assertEqual(result.returncode, 0, diagnostics(result))
        self.assertTrue((self.root / '--events-file').is_file(), diagnostics(result))

    def test_help_mentions_events_file(self):
        result = self.invoke([str(self.preview), '--help'])
        self.assertEqual(result.returncode, 0, diagnostics(result))
        self.assertIn('[--events-file <path>]', result.stdout, diagnostics(result))


if __name__ == '__main__':
    start = time.perf_counter()
    result = unittest.main(argv=[sys.argv[0]], verbosity=2, exit=False).result
    wall = time.perf_counter() - start
    rss = max(resource.getrusage(who).ru_maxrss
              for who in (resource.RUSAGE_SELF, resource.RUSAGE_CHILDREN))
    rss_mib = rss / (1024**2 if sys.platform == 'darwin' else 1024)
    print(f'MEASURE wall_s={wall:.6f} rss_mib={rss_mib:.3f}', flush=True)
    within_budget = wall <= 30 and rss_mib <= 256
    if not within_budget:
        print('preview_event_stream exceeded wall <= 30 s / RSS <= 256 MiB', file=sys.stderr)
    sys.exit(0 if result.wasSuccessful() and within_budget else 1)
