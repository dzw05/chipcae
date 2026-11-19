# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
from pathlib import Path
import tempfile
import unittest
from partitions_fixtures import fixture, save, success


class Absent(unittest.TestCase):
    def test_original_fixture_preserves_prechange_bytes_and_is_repeatable(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp) / 'run'
            _, manifest = fixture(run)
            self.assertNotIn('partitions', manifest)
            _, first = success(self, run)
            self.assertEqual(success(self, run)[1], first)

    def test_top_level_names_do_not_enable_partition_validation(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp) / 'run'
            _, manifest = fixture(run)
            manifest.update(per_step=True, solver_fixed_per_run='invalid', Partitions=[])
            save(run, manifest)
            success(self, run)


if __name__ == '__main__':
    unittest.main(argv=[__file__])
