# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
"""Shared synthetic inputs and Type/Index protocol assertions."""
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(sys.argv[1]).resolve()
sys.path.insert(0, str(ROOT / 'tests/unit/result_summary'))
# The unchanged fixture exits exactly 77 if h5py or numpy is unavailable.
from fixtures import baseline_for_numpy, fixture, fixed_keys, h5py, np

sys.path.insert(0, str(ROOT))
from tools.vv import result_summary as rs

SCRIPT = ROOT / 'tools/vv/result_summary.py'

# Captured from the unchanged CLI at 5346215 before adding these tests.
# Keep the independent reference text, including non-ASCII text and final newline.
BASELINE_TEXT = '''{
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
'''
BASELINE_TEXT = baseline_for_numpy(BASELINE_TEXT)
BASELINE = BASELINE_TEXT.encode('utf-8')


def replace_attr(block, name, value, dtype=None):
    """Recreate instead of modifying, retaining deliberately bad shapes/dtypes."""
    if name in block.attrs:
        del block.attrs[name]
    block.attrs.create(name, value, dtype=dtype)


def cli(run):
    return subprocess.run([sys.executable, str(SCRIPT), str(run)],
                          cwd=run.parent, capture_output=True, timeout=20)


def serialized(result):
    return (json.dumps(result, indent=2, sort_keys=False, ensure_ascii=False,
                       allow_nan=False) + '\n').encode('utf-8')


def success(test, run, expected=BASELINE):
    completed = cli(run)
    test.assertEqual(completed.returncode, 0, completed.stderr.decode())
    test.assertEqual(completed.stderr, b'')
    result = json.loads(completed.stdout)
    fixed_keys(test, result)
    test.assertEqual(completed.stdout, serialized(result))
    if expected is not None:
        test.assertEqual(completed.stdout, expected)
    api_result = rs.summarize(run)
    fixed_keys(test, api_result)
    test.assertEqual(serialized(api_result), completed.stdout)
    return result, completed.stdout


def invalid(test, run, group, *messages):
    # Separate subtests ensure CLI and API are both exercised during the red pass.
    prefix = group + ': '
    with test.subTest(interface='CLI'):
        completed = cli(run)
        diagnostic = completed.stderr.decode('utf-8')
        test.assertEqual(completed.returncode, 14,
                         f'expected INPUT_INVALID for {group} {messages}; '
                         f'got rc={completed.returncode}, stderr={diagnostic!r}')
        test.assertEqual(completed.stdout, b'')
        test.assertEqual(len(completed.stderr.splitlines()), 1, diagnostic)
        test.assertTrue(diagnostic.startswith('result_summary: INPUT_INVALID: ' + prefix),
                        diagnostic)
        test.assertNotIn('Traceback', diagnostic)
        for message in messages:
            test.assertIn(message, diagnostic)
    with test.subTest(interface='API'):
        with test.assertRaises(rs.ResultSummaryError) as caught:
            rs.summarize(run)
        error = caught.exception
        test.assertIsInstance(error, rs.ResultSummaryError)
        test.assertEqual(error.code, 14)
        test.assertEqual(error.name, 'INPUT_INVALID')
        test.assertTrue(str(error).startswith(prefix), str(error))
        for message in messages:
            test.assertIn(message, str(error))
