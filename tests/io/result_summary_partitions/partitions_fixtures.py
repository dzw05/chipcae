# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
"""Synthetic partition metadata and shared CLI/API acceptance assertions."""
import json
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'tests/unit/result_summary'))
# Import first to retain the shared fixture's dependency-missing exit code 77.
from fixtures import baseline_for_numpy, fixture, save, fixed_keys, h5py, np

sys.path.insert(0, str(ROOT))
from tools.vv import result_summary as rs

SCRIPT = ROOT / 'tools/vv/result_summary.py'
BLOCK_COUNTS = (('hex', 'NumberOfCells'), ('cut', 'NumberOfCells'),
                ('eb_surface', 'Polygons/NumberOfCells'))

# Captured from the unchanged CLI at 80dcd16 before writing these tests.
BASELINE = baseline_for_numpy('''{
  "schema": "chipcae.vv.result_summary/0.1",
  "step_file": "out/step-0.vtkhdf",
  "mesh_sha256": "synthetic-sha256",
  "blocks": {
    "hex": {
      "points": 11,
      "cells": 2,
      "cell_kind_counts": {
        "0": 1,
        "1": 1
      }
    },
    "cut": {
      "points": 0,
      "cells": 0,
      "type_counts": {}
    },
    "eb_surface": {
      "points": 7,
      "polygons": 3
    }
  },
  "layer_names": [
    "substrate",
    "die_attach",
    "Si"
  ],
  "material_names": [
    "铜",
    "silicon"
  ],
  "temperature": {
    "min": 300.0,
    "max": 309.0,
    "top_min": 301.0,
    "top_max": 309.0,
    "top_mean": 305.0
  },
  "warpage": {
    "manifest_warpage_pv_m": 0.1,
    "w_bfp_m": 0.1250000000000002,
    "rel_diff": 0.2500000000000019,
    "n_top_points": 9
  },
  "manifest_summary": {
    "T_max": 309.0,
    "warpage_pv": 0.1
  }
}
''').encode('utf-8')
# Each variant was independently captured and checked against these replacements.
BASELINE_NO_SURFACE = BASELINE.replace(b'"points": 7,\n      "polygons": 3',
                                       b'"points": 0,\n      "polygons": 0')
BASELINE_STEP4 = BASELINE.replace(b'out/step-0.vtkhdf', b'out/step-4.vtkhdf')
BASELINE_STEP9 = BASELINE.replace(b'out/step-0.vtkhdf', b'out/step-9.vtkhdf')


def two_steps(run):
    """Make steps n=4 (two partitions) and n=9 (one in every primary block)."""
    first, manifest = fixture(run)
    renamed = first.with_name('step-4.vtkhdf')
    first.rename(renamed)
    second = first.with_name('step-9.vtkhdf')
    shutil.copyfile(renamed, second)
    with h5py.File(second, 'r+') as f:
        for block, cells in BLOCK_COUNTS:
            group = f['VTKHDF/' + block]
            for name in ('NumberOfPoints', cells):
                total = sum(int(value) for value in group[name][...])
                del group[name]
                group[name] = np.array([total], dtype='int64')
    manifest['steps'] = [{'n': 4, 'file': 'out/step-4.vtkhdf'},
                         {'n': 9, 'file': 'out/step-9.vtkhdf'}]
    per_step = []
    for record in manifest['steps']:
        with h5py.File(run / record['file'], 'r') as f:
            counts = [f[f'VTKHDF/{block}/{cells}'].shape[0]
                      for block, cells in BLOCK_COUNTS]
        assert all(count == counts[0] for count in counts), record['file']
        per_step.append(counts[0])
    manifest['partitions']['per_step'] = per_step
    manifest['partitions']['solver_fixed_per_run'] = False
    save(run, manifest)
    return second, manifest


def cli(run, args=()):
    return subprocess.run([sys.executable, str(SCRIPT), str(run), *args],
                          cwd=run.parent, capture_output=True, timeout=20)


def api(run, step=None):
    return rs.summarize(run, step=step)


def serialized(result):
    return (json.dumps(result, indent=2, sort_keys=False, ensure_ascii=False,
                       allow_nan=False) + '\n').encode('utf-8')


def success(test, run, expected=BASELINE, args=(), step=None):
    completed = cli(run, args)
    test.assertEqual(completed.returncode, 0, completed.stderr.decode('utf-8'))
    test.assertEqual(completed.stderr, b'')
    result = api(run, step)
    cli_result = json.loads(completed.stdout)
    fixed_keys(test, cli_result)
    fixed_keys(test, result)
    test.assertEqual(completed.stdout, serialized(result))
    test.assertEqual(completed.stdout, expected)
    for key in ('partitions', 'per_step', 'solver_fixed_per_run'):
        test.assertNotIn(key, result)
        test.assertNotIn(key, cli_result)
        test.assertNotIn(('"' + key + '":').encode('utf-8'), completed.stdout)
    return result, completed.stdout


def invalid(test, run, prefix, *substrings, args=(), step=None):
    # Independent subtests run both interfaces even while the CLI assertion is red.
    with test.subTest(interface='CLI'):
        completed = cli(run, args)
        diagnostic = completed.stderr.decode('utf-8')
        test.assertEqual(completed.returncode, 14,
                         f'expected INPUT_INVALID for {prefix}{", ".join(substrings)}; '
                         f'got rc={completed.returncode}, stderr={diagnostic!r}')
        test.assertEqual(completed.stdout, b'')
        test.assertEqual(len(completed.stderr.splitlines()), 1, diagnostic)
        test.assertTrue(diagnostic.startswith('result_summary: INPUT_INVALID: ' + prefix),
                        diagnostic)
        for substring in substrings:
            test.assertIn(substring, diagnostic)
        test.assertNotIn('Traceback', diagnostic)
    with test.subTest(interface='API'):
        with test.assertRaises(rs.ResultSummaryError) as caught:
            api(run, step)
        error = caught.exception
        test.assertEqual(error.code, 14)
        test.assertEqual(error.name, 'INPUT_INVALID')
        message = str(error)
        test.assertTrue(message.startswith(prefix), message)
        for substring in substrings:
            test.assertIn(substring, message)
        test.assertEqual(len(message.splitlines()), 1, message)
        test.assertNotIn('Traceback', message)
