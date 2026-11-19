# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
from pathlib import Path
import tempfile
import unittest
from partitions_fixtures import (
    fixture, save, success, two_steps, h5py, BLOCK_COUNTS,
    BASELINE_NO_SURFACE, BASELINE_STEP4, BASELINE_STEP9,
)


class Valid(unittest.TestCase):
    def test_single_step_accepts_both_flags_without_changing_bytes(self):
        for fixed in (True, False):
            with self.subTest(fixed=fixed), tempfile.TemporaryDirectory() as tmp:
                run = Path(tmp) / 'run'
                _, manifest = fixture(run)
                _, before = success(self, run)
                manifest['partitions'] = {'per_step': [2], 'solver_fixed_per_run': fixed}
                save(run, manifest)
                _, first = success(self, run, before)
                self.assertEqual(success(self, run, before)[1], first)

    def test_extra_and_top_level_names_are_ignored(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp) / 'run'
            _, manifest = fixture(run)
            _, before = success(self, run)
            manifest['partitions'] = {'per_step': [2], 'solver_fixed_per_run': True,
                                      'extra': 1, 'Per_step': None,
                                      'Solver_fixed_per_run': 'invalid'}
            manifest.update(per_step=True, solver_fixed_per_run=[])
            save(run, manifest)
            success(self, run, before)

    def test_optional_surface_may_be_absent(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp) / 'run'
            path, manifest = fixture(run)
            with h5py.File(path, 'r+') as f:
                del f['VTKHDF/eb_surface']
            _, before = success(self, run, BASELINE_NO_SURFACE)
            manifest['partitions'] = {'per_step': [2], 'solver_fixed_per_run': True}
            save(run, manifest)
            success(self, run, before)

    def test_nonconsecutive_step_ids_use_selected_manifest_index(self):
        selections = (((), None, BASELINE_STEP4, 'out/step-4.vtkhdf'),
                      (('--step', '9'), 9, BASELINE_STEP9, 'out/step-9.vtkhdf'),
                      (('--step', 'last'), 'last', BASELINE_STEP9, 'out/step-9.vtkhdf'))
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp) / 'run'
            second, manifest = two_steps(run)
            with h5py.File(second, 'r') as f:
                for block, cells in BLOCK_COUNTS:
                    for name in ('NumberOfPoints', cells):
                        self.assertEqual(f['VTKHDF/' + block][name].shape, (1,))
            before = []
            for args, step, expected, filename in selections:
                with self.subTest(metadata='absent', step=step):
                    result, output = success(self, run, expected, args, step)
                    self.assertEqual(result['step_file'], filename)
                    before.append(output)
            manifest['partitions'] = {'per_step': [2, 1], 'solver_fixed_per_run': False}
            save(run, manifest)
            for index, (args, step, _, filename) in enumerate(selections):
                with self.subTest(metadata='present', step=step):
                    result, _ = success(self, run, before[index], args, step)
                    self.assertEqual(result['step_file'], filename)


if __name__ == '__main__':
    unittest.main(argv=[__file__])
