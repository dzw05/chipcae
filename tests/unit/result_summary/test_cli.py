# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from fixtures import fixture, save, SCRIPT, h5py
from tools.vv.result_summary import summarize


class CLI(unittest.TestCase):
    def test_large_manifest_integer_round_trip(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp) / 'run'
            _, manifest = fixture(run)
            # JSON integers need no float conversion, even beyond float64 range.
            value = 10**400
            manifest['summary'].append(dict(name='large_count', value=value))
            save(run, manifest)
            self.assertEqual(summarize(run)['manifest_summary']['large_count'], value)
            command = [sys.executable, str(SCRIPT), str(run)]
            for write_output in (False, True):
                with self.subTest(write_output=write_output):
                    output = Path(tmp) / 'summary.json'
                    args = ['-o', str(output)] if write_output else []
                    result = subprocess.run(command + args, cwd=tmp,
                                            capture_output=True, timeout=20)
                    self.assertEqual(result.returncode, 0, result.stderr.decode())
                    self.assertEqual(result.stderr, b'')
                    if write_output:
                        self.assertEqual(result.stdout, b'')
                    content = output.read_bytes() if write_output else result.stdout
                    self.assertEqual(json.loads(content)['manifest_summary']['large_count'], value)

    def test_failure_stderr_contract(self):
        for case in ('invalid_manifest', 'missing_manifest', 'invalid_tolerance'):
            with self.subTest(case=case), tempfile.TemporaryDirectory() as tmp:
                run = Path(tmp) / 'run'
                _, manifest = fixture(run)
                args = []
                if case == 'invalid_manifest':
                    manifest['schema_version'] = 'unsupported'
                    save(run, manifest)
                    code, name = 14, 'INPUT_INVALID'
                    reason = 'expected manifest schema chipcae.results/1.0'
                elif case == 'missing_manifest':
                    (run / 'manifest.json').unlink()
                    code, name = 12, 'INPUT_REFERENCE_MISSING'
                    reason = str(FileNotFoundError(2, 'No such file or directory',
                                                   str(run / 'manifest.json')))
                else:
                    args = ['--top-rel-tol', '-1']
                    code, name = 14, 'INPUT_INVALID'
                    reason = 'top_rel_tol must be finite and nonnegative'
                result = subprocess.run(
                    [sys.executable, str(SCRIPT), str(run)] + args,
                    cwd=tmp, capture_output=True, timeout=20)
                self.assertEqual(result.returncode, code)
                self.assertEqual(result.stdout, b'')
                self.assertEqual(result.stderr,
                                 f'result_summary: {name}: {reason}\n'.encode('utf-8'))

    def test_partial_temperature_available(self):
        for missing in ('hex', 'cut'):
            for write_output in (False, True):
                with self.subTest(missing=missing, write_output=write_output), tempfile.TemporaryDirectory() as tmp:
                    run = Path(tmp) / 'run'
                    step, _ = fixture(run, cut=True)
                    with h5py.File(step, 'r+') as f:
                        del f[f'VTKHDF/{missing}/PointData/T']
                    output = Path(tmp) / 'summary.json'
                    args = ['-o', str(output)] if write_output else []
                    result = subprocess.run(
                        [sys.executable, str(SCRIPT), str(run)] + args,
                        cwd=tmp, capture_output=True, timeout=20)
                    self.assertEqual(result.returncode, 0, result.stderr.decode())
                    self.assertEqual(result.stderr, b'')
                    if write_output:
                        self.assertEqual(result.stdout, b'')
                    content = output.read_bytes() if write_output else result.stdout
                    expected = dict(min=100., max=999., top_min=999., top_max=999., top_mean=999.) if missing == 'hex' else dict(
                        min=300., max=309., top_min=301., top_max=309., top_mean=305.)
                    self.assertEqual(json.loads(content)['temperature'], expected)
                    self.assertEqual(json.loads(content), summarize(run))

    def test_bytes_and_repeatability(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp)/'run'
            step, _ = fixture(run)
            command = [sys.executable, str(SCRIPT), str(run)]
            def invoke(args):
                result = subprocess.run(command + args, cwd=tmp, capture_output=True, timeout=20)
                self.assertEqual(result.returncode, 0, result.stderr.decode())
                self.assertEqual(result.stderr, b'')
                return result.stdout
            first = invoke([])
            self.assertEqual(first, invoke([]))
            output = Path(tmp)/'summary.json'
            self.assertEqual(invoke(['-o', str(output)]), b'')
            self.assertEqual(first, output.read_bytes())
            # The brief freezes the byte-for-byte JSON layout as a CLI contract.
            expected = json.dumps(summarize(run), indent=2, sort_keys=False, ensure_ascii=False)+'\n'
            self.assertEqual(first, expected.encode('utf-8'))
            self.assertTrue(first.endswith(b'\n'))
            with h5py.File(step, 'r+') as f:
                block = f['VTKHDF/hex']
                points = block['Points'][...]
                center = (points[:, 0] == 0.) & (points[:, 1] == 0.) & (points[:, 2] == 1.)
                points[center, 2] = 1. - 5e-13
                block['Points'][...] = points
                temperatures = block['PointData/T'][...]
                temperatures[center] = 1000.
                block['PointData/T'][...] = temperatures
            default = json.loads(invoke([]))
            exact = json.loads(invoke(['--top-rel-tol', '0']))
            self.assertEqual(default['warpage']['n_top_points'], 9)
            self.assertEqual(exact['warpage']['n_top_points'], 8)
            self.assertEqual(default['temperature']['top_max'], 1000.)
            self.assertEqual(exact['temperature']['top_max'], 309.)
            self.assertNotEqual(default, exact)


if __name__ == '__main__':
    unittest.main(argv=[__file__])
