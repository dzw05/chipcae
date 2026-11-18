# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
"""Synthetic inputs and protocol assertions for result-summary hardening."""
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'tests/unit/result_summary'))
# Import the existing fixture first: unavailable h5py/numpy must return 77.
from fixtures import fixture, save, fixed_keys, h5py, np, SCRIPT
from tools.vv import result_summary as rs


def cli(run):
    return subprocess.run([sys.executable, str(SCRIPT), str(run)],
                          cwd=run.parent, capture_output=True, timeout=20)


def invalid_cli(test, completed, *messages):
    test.assertEqual(completed.returncode, 14, completed.stderr.decode())
    test.assertEqual(completed.stdout, b'')
    test.assertEqual(len(completed.stderr.splitlines()), 1, completed.stderr.decode())
    test.assertTrue(completed.stderr.startswith(b'result_summary: INPUT_INVALID: '),
                    completed.stderr.decode())
    test.assertNotIn(b'Traceback', completed.stderr)
    for message in messages:
        test.assertIn(message.encode('utf-8'), completed.stderr)


def invalid_api(test, run, *messages):
    with test.assertRaises(rs.ResultSummaryError) as caught:
        rs.summarize(run)
    test.assertEqual(caught.exception.code, 14)
    test.assertEqual(caught.exception.name, 'INPUT_INVALID')
    for message in messages:
        test.assertIn(message, str(caught.exception))
    return str(caught.exception)


def success(test, run):
    completed = cli(run)
    test.assertEqual(completed.returncode, 0, completed.stderr.decode())
    test.assertEqual(completed.stderr, b'')
    result = rs.summarize(run)
    fixed_keys(test, result)
    expected = json.dumps(result, indent=2, sort_keys=False, ensure_ascii=False,
                          allow_nan=False) + '\n'
    test.assertEqual(completed.stdout, expected.encode('utf-8'))
    return result


def set_pv(run, manifest, value):
    entry = next(e for e in manifest['summary'] if e['name'] == 'warpage_pv')
    entry['value'] = value
    entry['unit'] = 'm'
    save(run, manifest)


def overflow_displacement(step):
    """Assign equal copies the same sign, alternating over unique top coordinates."""
    with h5py.File(step, 'r+') as f:
        block = f['VTKHDF/hex']
        points = block['Points'][...]
        top = np.flatnonzero(points[:, 2] == points[:, 2].max())
        _, inverse = np.unique(points[top], axis=0, return_inverse=True)
        u = block['PointData/U'][...]
        u[top, 2] = np.where(inverse % 2 == 0, 1e308, -1e308)
        assert np.isfinite(u).all(), 'overflow fixture must contain only finite inputs'
        block['PointData/U'][...] = u


def overflow_temperature(step):
    with h5py.File(step, 'r+') as f:
        for name in ('hex', 'cut'):
            values = f[f'VTKHDF/{name}/PointData/T']
            values[...] = np.full(values.shape, 1e308)
            assert np.isfinite(values[...]).all(), 'temperature inputs must be finite'
