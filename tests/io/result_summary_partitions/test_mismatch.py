# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
from pathlib import Path
import tempfile
import unittest
from partitions_fixtures import (
    fixture, save, invalid, two_steps, rs, h5py, np, BLOCK_COUNTS,
)


class Mismatch(unittest.TestCase):
    def test_manifest_counts_compare_partition_length_instead_of_sum(self):
        for count in (1, 3):
            with self.subTest(count=count), tempfile.TemporaryDirectory() as tmp:
                run = Path(tmp) / 'run'
                path, manifest = fixture(run)
                manifest['partitions'] = {'per_step': [count], 'solver_fixed_per_run': True}
                save(run, manifest)
                with h5py.File(path, 'r') as f:
                    rs._read(f['VTKHDF'])
                invalid(self, run, 'partitions.per_step[0]: ', 'partition count mismatch',
                        '/VTKHDF/hex/NumberOfCells', f'expected {count}', 'got 2')

    def test_appended_empty_partition_is_checked_in_every_primary_block(self):
        for block, cells in BLOCK_COUNTS:
            with self.subTest(block=block), tempfile.TemporaryDirectory() as tmp:
                run = Path(tmp) / 'run'
                path, manifest = fixture(run)
                manifest['partitions'] = {'per_step': [2], 'solver_fixed_per_run': True}
                save(run, manifest)
                with h5py.File(path, 'r+') as f:
                    group = f['VTKHDF/' + block]
                    for name in ('NumberOfPoints', cells):
                        original = group[name][...]
                        del group[name]
                        group[name] = np.array([*original, 0], dtype='int64')
                        self.assertEqual(int(group[name][...].sum()), int(original.sum()))
                        self.assertEqual(group[name].shape, (3,))
                    # This must pass all existing dtype, shape, topology and metadata checks.
                    rs._read(f['VTKHDF'])
                invalid(self, run, 'partitions.per_step[0]: ', 'partition count mismatch',
                        f'/VTKHDF/{block}/{cells}', 'expected 2', 'got 3')

    def test_selected_second_step_mismatch_reports_manifest_index_one(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp) / 'run'
            path, manifest = two_steps(run)
            manifest['partitions'] = {'per_step': [2, 2], 'solver_fixed_per_run': False}
            save(run, manifest)
            with h5py.File(path, 'r') as f:
                rs._read(f['VTKHDF'])
            for args, step in ((('--step', '9'), 9), (('--step', 'last'), 'last')):
                with self.subTest(step=step):
                    invalid(self, run, 'partitions.per_step[1]: ', 'partition count mismatch',
                            '/VTKHDF/hex/NumberOfCells', 'expected 2', 'got 1',
                            args=args, step=step)


if __name__ == '__main__':
    unittest.main(argv=[__file__])
