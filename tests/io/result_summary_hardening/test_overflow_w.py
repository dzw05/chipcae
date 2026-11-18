# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
from pathlib import Path
import tempfile
import unittest
from hardening_fixtures import (
    fixture, set_pv, overflow_displacement, cli, invalid_cli, invalid_api,
)


class OverflowWarpage(unittest.TestCase):
    def test_finite_displacements_report_warpage_path_repeatably(self):
        # Integer PV must not send an infinite w into Fraction before validation.
        for pv in (.1, 1):
            with self.subTest(pv=pv), tempfile.TemporaryDirectory() as tmp:
                run = Path(tmp) / 'run'
                step, manifest = fixture(run)
                set_pv(run, manifest, pv)
                overflow_displacement(step)
                first, second = cli(run), cli(run)
                self.assertEqual(first.stderr, second.stderr)
                for index, completed in enumerate((first, second)):
                    with self.subTest(interface='CLI', invocation=index):
                        invalid_cli(self, completed, 'warpage.w_bfp_m', 'non-finite')
                with self.subTest(interface='API'):
                    invalid_api(self, run, 'warpage.w_bfp_m', 'non-finite')


if __name__ == '__main__':
    unittest.main(argv=[__file__])
