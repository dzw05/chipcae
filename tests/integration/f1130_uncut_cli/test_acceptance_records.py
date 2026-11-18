"""Regression checks for acceptance bookkeeping and the failure exit path."""

from contextlib import redirect_stderr, redirect_stdout
from io import StringIO
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

import test_uncut_cli_e2e as e2e


CHECKS = ('import', 'fractions', 'preview', 'solve_readback', 'inputs_unchanged', 'numerical')


class AcceptanceRecords(unittest.TestCase):
    def write_record(self, root, checks, failures=(), ranks=1, parallel_required=False):
        measurements = dict(
            acceptance_checks=checks, parallel_required=parallel_required,
            qoi=dict(warpage_pv=1e-6, U_mag_max=2e-6, T_max=318.15),
            max_temperature_error_K=1e-10)
        with redirect_stdout(StringIO()):
            e2e.write_result(root, ranks, measurements, {}, [], list(failures))
        return json.loads((root / 'result.json').read_text(encoding='utf-8'))

    def test_each_missing_acceptance_check_prevents_pass(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for missing in CHECKS:
                with self.subTest(missing=missing):
                    checks = {name: True for name in CHECKS if name != missing}
                    result = self.write_record(root, checks)
                    self.assertEqual(result['verdict'], 'fail')
                    self.assertTrue(any(missing in note for note in result['notes']))

    def test_empty_numerical_record_cannot_pass(self):
        with tempfile.TemporaryDirectory() as directory, redirect_stdout(StringIO()):
            root = Path(directory)
            e2e.write_result(root, 1, {}, {}, [], [])
            result = json.loads((root / 'result.json').read_text(encoding='utf-8'))
            self.assertEqual(result['verdict'], 'fail')

    def test_parallel_comparison_is_required_for_both_available_runs(self):
        with tempfile.TemporaryDirectory() as directory:
            for ranks in (1, 2):
                with self.subTest(ranks=ranks):
                    result = self.write_record(Path(directory), dict.fromkeys(CHECKS, True),
                                               ranks=ranks, parallel_required=True)
                    self.assertEqual(result['verdict'], 'fail')

    def test_failed_run_does_not_change_another_runs_verdict(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            checks = dict.fromkeys(CHECKS, True)
            for failure in ('temperature error', 'readback mismatch', 'invalid QoI'):
                with self.subTest(failure=failure):
                    failed = self.write_record(root, checks, [failure])
                    passed = self.write_record(root, checks)
                    self.assertEqual(failed['verdict'], 'fail')
                    self.assertEqual(passed['verdict'], 'pass')

    def test_main_raises_accumulated_failures_after_writing_gated_results(self):
        try:
            import numpy  # noqa: F401
            import h5py  # noqa: F401
        except ImportError:
            self.skipTest('main requires numpy/h5py')
        output = StringIO()
        with tempfile.TemporaryDirectory() as directory:
            absent = str(Path(directory) / 'absent-executable')
            env = dict(CHIPCAE_IMPORT_EXE=absent, CHIPCAE_PREVIEW_EXE=absent,
                       CHIPCAE_SOLVE_EXE=absent)
            with patch.dict(e2e.os.environ, env), redirect_stdout(output), redirect_stderr(StringIO()):
                with self.assertRaises(AssertionError):
                    e2e.main()
        record = json.loads(output.getvalue().split('RESULT ranks1/result.json\n', 1)[1])
        self.assertEqual(record['verdict'], 'fail')
        self.assertEqual(record['qoi'], {})

    def test_numerical_check_completes_only_after_all_inspection_assertions(self):
        try:
            import numpy as np
            import h5py
        except ImportError:
            self.skipTest('inspection requires numpy/h5py')
        # One 1 mm FR4 layer: R=1/300+1/1000, q=150000/13 W/m².
        # T(top)=298.15+500/13 K, independently evaluated here as a literal.
        for failure in (None, 'temperature', 'readback', 'qoi'):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                (root / 'out').mkdir()
                warpage = -1e-6 if failure == 'qoi' else 1e-6
                e2e.write_json(root / 'manifest.json', dict(
                    schema_version='chipcae.results/1.0', summary=[
                        dict(name='warpage_pv', value=warpage),
                        dict(name='U_mag_max', value=2e-6), dict(name='T_max', value=336.62)]))
                e2e.write_json(root / 'summary.json', dict(
                    warpage=dict(w_bfp_m=1.01e-6 if failure == 'readback' else 1e-6)))
                with h5py.File(root / 'out/step-0.vtkhdf', 'w') as file:
                    file['VTKHDF/cut/NumberOfCells'] = [0]
                    file['VTKHDF/hex/NumberOfCells'] = [1]
                    file['VTKHDF/hex/CellData/cell_kind'] = [0]
                    file['VTKHDF/hex/Points'] = [[0., 0., 0.], [.001, .001, .001]]
                    top = 336.61153846153843 + (.001 if failure == 'temperature' else 0)
                    file['VTKHDF/hex/PointData/T'] = [298.15, top]
                measurements = dict(acceptance_checks={})
                stack = dict(layers=[dict(material='mat_fr4_eq', thickness=.001)])
                with redirect_stdout(StringIO()):
                    if failure is None:
                        e2e.inspect_run(root, stack, 1, measurements, np, h5py)
                    else:
                        with self.assertRaises(AssertionError):
                            e2e.inspect_run(root, stack, 1, measurements, np, h5py)
                self.assertEqual(measurements['acceptance_checks'].get('numerical', False),
                                 failure is None)


if __name__ == '__main__':
    unittest.main(argv=[sys.argv[0]])
