# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
"""Synthetic diagnostic fields and CLI assertions for result-summary acceptance."""
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'tests/unit/result_summary'))
# Import first so missing h5py/numpy retains the shared fixture's exit code 77.
from fixtures import fixture, save, fixed_keys, SCRIPT

import h5py
import numpy as np


def diagnostics(cells, dtype='float64'):
    # Finite copper fractions outside [0, 1] are legal under this contract.
    return {
        'copper_fraction': np.linspace(-0.25, 1.25, cells, dtype=dtype),
        'conductivity_diag': np.arange(cells * 3, dtype=dtype).reshape(cells, 3),
    }


def insert(step, block, **fields):
    with h5py.File(step, 'a') as handle:
        cell_data = handle[f'VTKHDF/{block}'].require_group('CellData')
        for name, values in fields.items():
            cell_data.create_dataset(name, data=values)


def cli(run):
    return subprocess.run([sys.executable, str(SCRIPT), str(run)],
                          cwd=run.parent, capture_output=True, timeout=20)


def success(test, run, expected=None):
    completed = cli(run)
    test.assertEqual(completed.returncode, 0, completed.stderr.decode('utf-8'))
    test.assertEqual(completed.stderr, b'')
    fixed_keys(test, json.loads(completed.stdout))
    if expected is not None:
        test.assertEqual(completed.stdout, expected)
    return completed.stdout


def baseline(test, run, step):
    with h5py.File(step, 'r') as handle:
        for block in ('hex', 'cut', 'eb_surface'):
            for field in ('copper_fraction', 'conductivity_diag'):
                test.assertNotIn(f'CellData/{field}', handle[f'VTKHDF/{block}'])
    return success(test, run)


def invalid(test, run, block, field, phrase):
    first, second = cli(run), cli(run)
    test.assertEqual(first.returncode, second.returncode)
    test.assertEqual(first.stderr, second.stderr)
    path = f'/VTKHDF/{block}/CellData/{field}:'
    for completed in (first, second):
        diagnostic = completed.stderr.decode('utf-8')
        test.assertEqual(completed.returncode, 14,
                         f'expected INPUT_INVALID for {path} {phrase}; '
                         f'got rc={completed.returncode}, stderr={diagnostic!r}')
        test.assertEqual(completed.stdout, b'')
        test.assertEqual(len(completed.stderr.splitlines()), 1, diagnostic)
        test.assertTrue(diagnostic.startswith('result_summary: INPUT_INVALID: '),
                        diagnostic)
        test.assertIn(path, diagnostic)
        test.assertIn(phrase, diagnostic)
