# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
"""Shared acceptance helpers; all result data lives in temporary directories."""
import contextlib
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'tests/unit/result_summary'))
# Import this first: its dependency check exits 77 without h5py/numpy.
from fixtures import fixture, save, h5py
sys.path.insert(0, str(ROOT / 'tests/io/result_summary_steps'))
from steps_fixtures import steps_fixture
from tools.vv.result_summary import ResultSummaryError, summarize
from tools.vv.check_result_data_class import check_file
# Deliberately missing in the test-first commit (ImportError, not a fixture error).
import tools.vv.result_summary_result as exporter

SCRIPT = ROOT / 'tools/vv/result_summary_result.py'
QOI_KEYS = ['step_file', 'mesh_sha256', 'blocks', 'layer_names',
            'material_names', 'temperature', 'warpage', 'manifest_summary']
TOP_KEYS = ['schema', 'case_id', 'data_class', 'label', 'date', 'commit',
            'qoi', 'verdict']
METADATA = dict(case_id='RS-RJ_1.0', data_class='synthetic', label='实测',
                date='20261118', commit='abcdef1234567', verdict='pass')


def cli_args(run, **overrides):
    metadata = dict(METADATA, **overrides)
    args = [str(run)]
    for key, value in metadata.items():
        if value is not None:
            args.extend(('--' + key.replace('_', '-'), str(value)))
    return args


def cli(run, *extra, env=None, **overrides):
    child_env = os.environ.copy()
    child_env.pop('CHIPCAE_GIT_SHA', None)
    if env is not None:
        child_env.update(env)
    # Running outside the repository also exercises direct-script imports.
    return subprocess.run([sys.executable, str(SCRIPT),
                           *cli_args(run, **overrides), *extra],
                          cwd=run.parent, env=child_env,
                          capture_output=True, timeout=20)


def protocol_cli(run, *extra, **overrides):
    """Call the public main for error matrices and controlled external failures."""
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        try:
            code = exporter.main([*cli_args(run, **overrides), *extra])
        except SystemExit as exc:
            code = exc.code
    return code, out.getvalue(), err.getvalue()


def encoded(obj):
    return (json.dumps(obj, indent=2, sort_keys=False,
                       ensure_ascii=False, allow_nan=False) + '\n').encode('utf-8')


class ExportCase(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.base = Path(temporary.name)
        self.run = self.base / 'run'
        self.run.mkdir()
        self.step, self.manifest = fixture(self.run)
        self.target = self.base / 'result.json'

    def object(self, summary=None, **overrides):
        if summary is None:
            summary = summarize(self.run)
        return exporter.result_json_from_summary(
            summary, **dict(METADATA, **overrides))

    def assert_qoi(self, obj, step=None, top_rel_tol=1e-12):
        summary = summarize(self.run, top_rel_tol=top_rel_tol, step=step)
        self.assertEqual(list(obj['qoi']), QOI_KEYS)
        # The brief requires exact copying, including nested floats and nulls.
        for key in QOI_KEYS:
            self.assertEqual(obj['qoi'][key], summary[key], key)

    def assert_failure(self, result, field=None, code=14):
        actual, out, err = result
        self.assertEqual(actual, code, err)
        self.assertEqual(out, '')
        name = 'INPUT_INVALID' if code == 14 else 'INPUT_REFERENCE_MISSING'
        self.assertTrue(err.startswith(f'result_summary_result: {name}: '), err)
        self.assertTrue(err.endswith('\n'), err)
        self.assertEqual(len(err.splitlines()), 1, err)
        self.assertNotIn('Traceback', err)
        if field is not None:
            # Accept API names, CLI switches, or nested field paths.
            normalized = err.replace('-', '_').replace('.', '_')
            self.assertIn(field.replace('-', '_'), normalized)

    def assert_api_invalid(self, field, **overrides):
        with self.assertRaises(exporter.ResultJsonError) as caught:
            self.object(**overrides)
        self.assertEqual(caught.exception.code, 14)
        self.assertEqual(caught.exception.name, 'INPUT_INVALID')
        self.assertIn(field, str(caught.exception))
