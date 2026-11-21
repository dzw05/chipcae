"""Real-case output and failures stay in a persistent local run directory."""
from contextlib import redirect_stderr, redirect_stdout
import io
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

import parts_support as support


class RealRunPrivacy(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix='sp7-private-regression-')
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        env = patch.dict(os.environ, {'CHIPCAE_REAL_RUNS_DIR': str(self.root)})
        env.start()
        self.addCleanup(env.stop)

    def invoke(self, test):
        output = io.StringIO()
        with redirect_stdout(output), redirect_stderr(output):
            support.main(test, needs_occt=False, local_runs=True)
        return output.getvalue()

    def test_cli_output_is_local_and_run_survives_completion(self):
        def acceptance(d):
            # Exercise the actual subprocess capture/print path with synthetic data.
            with patch.object(support, 'cli', return_value=sys.executable):
                support.run('solve', ['-c',
                    "import sys; print('warpage_pv synthetic-value'); "
                    "print('private-stderr', file=sys.stderr)"])
            print('MEASURED real-bulk synthetic-value')
            (d / 'retained.txt').write_text('synthetic fixture')

        output = self.invoke(acceptance)
        self.assertNotIn('synthetic-value', output)
        self.assertNotIn('private-stderr', output)
        runs = list(self.root.iterdir())
        self.assertEqual(len(runs), 1)
        self.assertEqual((runs[0] / 'retained.txt').read_text(), 'synthetic fixture')
        log = (runs[0] / 'run.log').read_text()
        self.assertIn('warpage_pv synthetic-value', log)
        self.assertIn('private-stderr', log)
        self.assertIn('MEASURED real-bulk synthetic-value', log)

    def test_failure_details_stay_local(self):
        def acceptance(d):
            raise AssertionError('warpage_pv synthetic-failure-value')

        with self.assertRaises(AssertionError) as error:
            self.invoke(acceptance)
        self.assertNotIn('synthetic-failure-value', str(error.exception))
        run, = self.root.iterdir()
        self.assertIn('synthetic-failure-value', (run / 'run.log').read_text())
        self.assertIn(str(run / 'run.log'), str(error.exception))

    def test_dependency_skip_does_not_expose_private_details(self):
        def acceptance(d):
            raise support.DependencyMissing('private synthetic-dependency-value')

        with patch.dict(os.environ, {'SP7_STRICT_ACCEPTANCE': '0'}):
            output = io.StringIO()
            with redirect_stdout(output), redirect_stderr(output):
                with self.assertRaises(SystemExit) as error:
                    support.main(acceptance, needs_occt=False, local_runs=True)
        self.assertEqual(error.exception.code, 77)
        self.assertNotIn('synthetic-dependency-value', output.getvalue())
        run, = self.root.iterdir()
        self.assertIn('synthetic-dependency-value', (run / 'run.log').read_text())


if __name__ == '__main__':
    unittest.main(argv=[__file__])
