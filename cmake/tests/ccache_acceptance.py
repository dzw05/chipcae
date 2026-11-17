"""Hermetic E7 acceptance fixtures; run via the adjacent cmake -P entry point."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[2]
SHIM = Path('/workspace/tasks/shim')
LAUNCHERS = ('CMAKE_C_COMPILER_LAUNCHER', 'CMAKE_CXX_COMPILER_LAUNCHER')


class Acceptance(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='e7-acceptance-')
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.bin = self.root / 'bin'
        self.bin.mkdir()
        self.env = os.environ.copy()
        for key in (*LAUNCHERS, 'CCACHE_DIR', 'BASH_ENV', 'CHIPCAE_SLOT_HELD'):
            self.env.pop(key, None)
        # Only explicitly selected tools exist after the mandatory slot wrappers.
        search = os.pathsep.join(p for p in os.get_exec_path() if Path(p) != SHIM)
        for tool in ('cmake', 'ninja', 'basename', 'nice', 'mkdir', 'flock', 'date', 'sleep'):
            real = shutil.which(tool, path=search)
            self.assertIsNotNone(real, f'fixture requires {tool}')
            (self.bin / tool).symlink_to(real)
        self.env['PATH'] = f'{SHIM}:{self.bin}'

    def fake_ccache(self):
        fake = self.bin / 'ccache'
        fake.write_text('#!/bin/sh\nprintf "called\\n" >> "' + str(self.root / 'calls') + '"\n')
        fake.chmod(0o755)
        return str(fake)

    def configure(self, presets):
        source = self.root / 'src'
        source.mkdir(exist_ok=True)
        # OPTIONAL deliberately lets the baseline configure, then fail a value assertion.
        (source / 'CMakeLists.txt').write_text(
            'cmake_minimum_required(VERSION 3.20)\nproject(cache_probe NONE)\n'
            f'include("{REPO}/cmake/ChipcaeCompilerCache.cmake" OPTIONAL)\n'
            'file(WRITE "${CMAKE_BINARY_DIR}/launchers.txt" '
            '"${CMAKE_C_COMPILER_LAUNCHER}\\n${CMAKE_CXX_COMPILER_LAUNCHER}\\n")\n')
        build = self.root / ('build-' + str(len(list(self.root.glob('build-*')))))
        env = self.env.copy()
        # Isolate CMake module behavior from future shim launcher defaults.
        env.update(dict.fromkeys(LAUNCHERS, ''))
        result = subprocess.run(
            ['cmake', '-S', str(source), '-B', str(build), '-G', 'Ninja',
             f'-DCMAKE_MAKE_PROGRAM={self.bin}/ninja',
             '-DCMAKE_FIND_USE_CMAKE_SYSTEM_PATH=FALSE',
             '-DCMAKE_FIND_USE_INSTALL_PREFIX=FALSE',
             *[f'-D{k}={v}' for k, v in presets.items()]],
            env=env, text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return (build / 'launchers.txt').read_text().splitlines()

    def test_ccache_launcher_detected(self):
        fake = self.fake_ccache()
        for presets in ({}, dict.fromkeys(LAUNCHERS, '')):
            with self.subTest(presets=presets):
                self.assertEqual(self.configure(presets), [fake, fake],
                                 'ccache was not automatically enabled')

    def test_ccache_launcher_respects_preset(self):
        fake = self.fake_ccache()
        for presets, expected in (
            ({LAUNCHERS[1]: 'sccache'}, [fake, 'sccache']),
            ({LAUNCHERS[0]: '/caller/custom-cache'}, ['/caller/custom-cache', fake]),
            (dict(zip(LAUNCHERS, ['custom-c', 'sccache'])), ['custom-c', 'sccache']),
        ):
            with self.subTest(presets=presets):
                self.assertEqual(self.configure(presets), expected)

    def test_ccache_launcher_absent(self):
        self.assertIsNone(shutil.which('ccache', path=self.env['PATH']))
        self.assertEqual(self.configure({}), ['', ''])
        self.assertEqual(self.configure(dict(zip(LAUNCHERS, ['custom-c', 'sccache']))),
                         ['custom-c', 'sccache'])

    def test_shim_ccache_env(self):
        self.check_cache_env(['cmake'])

    def test_slot_ccache_env(self):
        self.check_cache_env(['/bin/bash', '/workspace/tasks/slot.sh', 'cmake'])

    def check_cache_env(self, entry):
        # Recorder replaces only the downstream tool, never the mandatory wrapper.
        recorder = self.bin / 'cmake'
        recorder.unlink()
        recorder.write_text(
            '#!' + shutil.which('python3') + '\nimport json, os, sys\n'
            'print(json.dumps({"args": sys.argv[1:], "env": dict(os.environ)}))\n')
        recorder.chmod(0o755)
        for present in (False, True):
            if present:
                self.fake_ccache()
            for preset in ({}, {'CCACHE_DIR': '/caller/cache', LAUNCHERS[0]: 'custom-c'},
                           {LAUNCHERS[1]: 'sccache'},
                           {'CCACHE_DIR': '/caller/cache', **dict.fromkeys(LAUNCHERS, 'custom')},
                           dict.fromkeys(('CCACHE_DIR', *LAUNCHERS), '')):
                with self.subTest(ccache=present, preset=preset):
                    env = self.env | preset
                    result = subprocess.run([*entry, '--build', 'original build', '--parallel', '2'],
                                            env=env, text=True, capture_output=True)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    recorded = json.loads(result.stdout)
                    self.assertEqual(recorded['args'], ['--build', 'original build', '--parallel', '2'])
                    self.assertTrue(recorded['env'].get('CHIPCAE_SLOT_HELD'))
                    for key in ('CCACHE_DIR', *LAUNCHERS):
                        expected = preset.get(key)
                        if present and expected is None:
                            expected = '/workspace/.ccache' if key == 'CCACHE_DIR' else 'ccache'
                        with self.subTest(variable=key):
                            self.assertEqual(recorded['env'].get(key), expected, key)


if __name__ == '__main__':
    unittest.main()
