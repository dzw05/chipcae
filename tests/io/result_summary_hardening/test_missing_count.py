# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
from pathlib import Path
import tempfile
import unittest
from hardening_fixtures import fixture, h5py, cli, invalid_cli, invalid_api


class MissingCount(unittest.TestCase):
    def test_missing_partition_counts_report_full_dataset_path(self):
        for path in ('/VTKHDF/eb_surface/Polygons/NumberOfCells',
                     '/VTKHDF/hex/NumberOfPoints'):
            with self.subTest(path=path), tempfile.TemporaryDirectory() as tmp:
                run = Path(tmp) / 'run'
                step, _ = fixture(run)
                with h5py.File(step, 'r+') as f:
                    del f[path]
                with self.subTest(interface='CLI'):
                    invalid_cli(self, cli(run), path, 'required field missing')
                with self.subTest(interface='API'):
                    invalid_api(self, run, path, 'required field missing')


if __name__ == '__main__':
    unittest.main(argv=[__file__])
