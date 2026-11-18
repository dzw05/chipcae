# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
from pathlib import Path
import tempfile
import unittest
from hardening_fixtures import fixture, set_pv, cli, invalid_cli, invalid_api


class SubnormalPV(unittest.TestCase):
    def test_computed_relative_difference_obeys_error_protocol(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp) / 'run'
            _, manifest = fixture(run)
            set_pv(run, manifest, 1e-320)
            with self.subTest(interface='CLI'):
                invalid_cli(self, cli(run), 'warpage.rel_diff', 'non-finite')
            with self.subTest(interface='API'):
                invalid_api(self, run, 'warpage.rel_diff', 'non-finite')


if __name__ == '__main__':
    unittest.main(argv=[__file__])
