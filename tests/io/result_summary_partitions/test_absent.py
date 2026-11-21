# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
from pathlib import Path
import tempfile
import unittest
from partitions_fixtures import fixture, save, invalid


class Absent(unittest.TestCase):
    def test_missing_partitions_requires_object(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp) / 'run'
            _, manifest = fixture(run)
            manifest.pop('partitions', None)
            save(run, manifest)
            invalid(self, run, 'partitions:', 'required object')

    def test_top_level_names_cannot_replace_required_partitions(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp) / 'run'
            _, manifest = fixture(run)
            manifest.pop('partitions', None)
            manifest.update(per_step=True, solver_fixed_per_run='invalid', Partitions=[])
            save(run, manifest)
            invalid(self, run, 'partitions:', 'required object')

    def test_null_partitions_requires_object(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp) / 'run'
            _, manifest = fixture(run)
            manifest['partitions'] = None
            save(run, manifest)
            invalid(self, run, 'partitions:', 'required object')


if __name__ == '__main__':
    unittest.main(argv=[__file__])
