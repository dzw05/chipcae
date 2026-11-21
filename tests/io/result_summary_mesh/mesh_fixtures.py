# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
"""Synthetic mesh inputs and CLI/API assertions for result-summary acceptance."""
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'tests/unit/result_summary'))
# Import first so missing h5py/numpy retains the shared fixture's exit code 77.
from fixtures import baseline_for_numpy, fixture, save, fixed_keys

sys.path.insert(0, str(ROOT))
from tools.vv import result_summary as rs

SCRIPT = ROOT / 'tools/vv/result_summary.py'
INTEGER_KEYS = ('nx0', 'ny0', 'L', 'padded_cells', 'nz')
VALID_INTEGERS = {'nx0': 4, 'ny0': 5, 'L': 2, 'padded_cells': 20, 'nz': 3}

# Captured from the unchanged CLI at a2d3944 for every successful acceptance input.
# Inputs with the same hash emitted identical bytes, including UTF-8 and newline.
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
# These two variants were also captured independently before any production edit.
BASELINE_NO_HASH = BASELINE.replace(b'"synthetic-sha256"', b'null')
BASELINE_EMPTY_HASH = BASELINE.replace(b'"synthetic-sha256"', b'""')


def cli(run):
    return subprocess.run([sys.executable, str(SCRIPT), str(run)],
                          cwd=run.parent, capture_output=True, timeout=20)


def serialized(result):
    return (json.dumps(result, indent=2, sort_keys=False, ensure_ascii=False,
                       allow_nan=False) + '\n').encode('utf-8')


def success(test, run, expected=BASELINE):
    completed = cli(run)
    test.assertEqual(completed.returncode, 0, completed.stderr.decode('utf-8'))
    test.assertEqual(completed.stderr, b'')
    result = rs.summarize(run)
    cli_result = json.loads(completed.stdout)
    fixed_keys(test, result)
    fixed_keys(test, cli_result)
    for key in INTEGER_KEYS:
        test.assertNotIn(key, result)
        test.assertNotIn(key, cli_result)
    test.assertEqual(completed.stdout, serialized(result))
    test.assertEqual(completed.stdout, expected)
    return result, completed.stdout


def invalid_integer(test, run, key):
    prefix = f'mesh.{key}: '
    # Independent subtests exercise both interfaces even during the failing pass.
    with test.subTest(interface='CLI'):
        completed = cli(run)
        diagnostic = completed.stderr.decode('utf-8')
        test.assertEqual(completed.returncode, 14,
                         f'expected INPUT_INVALID for {prefix}expected integer; '
                         f'got rc={completed.returncode}, stderr={diagnostic!r}')
        test.assertEqual(completed.stdout, b'')
        test.assertEqual(len(completed.stderr.splitlines()), 1, diagnostic)
        test.assertTrue(diagnostic.startswith('result_summary: INPUT_INVALID: ' + prefix),
                        diagnostic)
        test.assertIn('expected integer', diagnostic)
        test.assertNotIn('Traceback', diagnostic)
    with test.subTest(interface='API'):
        with test.assertRaises(rs.ResultSummaryError) as caught:
            rs.summarize(run)
        error = caught.exception
        test.assertEqual(error.code, 14)
        test.assertEqual(error.name, 'INPUT_INVALID')
        test.assertTrue(str(error).startswith(prefix), str(error))
        test.assertIn('expected integer', str(error))
