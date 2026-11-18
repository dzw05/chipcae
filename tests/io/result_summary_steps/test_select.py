# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
import tempfile
import unittest
from pathlib import Path
from steps_fixtures import (steps_fixture, save, success, api, fixed_keys,
                            STEP_SUMMARIES, assert_summary, assert_fields)


class Select(unittest.TestCase):
    def test_three_distinct_steps(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp)
            steps_fixture(run)
            warpages = {}
            for args, selector, n in [((), None, 0), (('--step', '0'), 0, 0),
                                      (('--step', '1'), 1, 1),
                                      (('--step', 'last'), 'last', 2),
                                      (('--step', '0001'), 1, 1)]:
                with self.subTest(args=args):
                    result, _ = success(self, run, args, selector)
                    self.assertEqual(result['step_file'], f'out/step-{n}.vtkhdf')
                    assert_summary(self, result, STEP_SUMMARIES[n])
                    assert_fields(self, result, n)
                    warpages[n] = result['warpage']['w_bfp_m']
            # Independent assertions above also check .125, .25, and .375.
            self.assertEqual(len(set(warpages.values())), 3)

    def test_api_selectors(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp)
            steps_fixture(run)
            for selector, n in [(None, 0), (0, 0), (1, 1), ('last', 2)]:
                with self.subTest(selector=selector):
                    result = api(self, run, selector)
                    fixed_keys(self, result)
                    self.assertEqual(result['step_file'], f'out/step-{n}.vtkhdf')
                    assert_summary(self, result, STEP_SUMMARIES[n])
                    assert_fields(self, result, n)

    def test_array_order_and_maximum_id(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp)
            manifest = steps_fixture(run)
            manifest['steps'] = [manifest['steps'][n] for n in (1, 2, 0)]
            save(run, manifest)
            for args, selector, n in [((), None, 1), (('--step', '0'), 0, 0),
                                      (('--step', 'last'), 'last', 2)]:
                with self.subTest(args=args):
                    result, _ = success(self, run, args, selector)
                    self.assertEqual(result['step_file'], f'out/step-{n}.vtkhdf')
                    assert_summary(self, result, STEP_SUMMARIES[n])
                    assert_fields(self, result, n)

    def test_legacy_and_tagged_entries_in_encounter_order(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp)
            manifest = steps_fixture(run)
            manifest['summary'] = [
                dict(name='legacy_count', value=10**400),
                dict(name='other', value=99, step=0),
                dict(name='U_mag_max', value=.02, step=1),
                dict(name='case_sensitive', value=17, Step=False),
                dict(name='warpage_pv', value=.4, unit='m', step=1),
                dict(name='T_max', value=320, step=1),
            ]
            save(run, manifest)
            result, _ = success(self, run, ('--step', '1'), 1)
            assert_summary(self, result, [('legacy_count', 10**400),
                           ('U_mag_max', .02), ('case_sensitive', 17),
                           ('warpage_pv', .4), ('T_max', 320)])
            assert_fields(self, result, 1)

    def test_raw_path_and_unselected_files(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp)
            manifest = steps_fixture(run)
            raw = './out/../out//step-0.vtkhdf'
            manifest['steps'][0]['file'] = raw
            # Unselected files must not be opened, even if absent or corrupt.
            (run / 'out/step-1.vtkhdf').unlink()
            (run / 'out/step-2.vtkhdf').write_text('not HDF5', encoding='utf-8')
            save(run, manifest)
            for args, selector in [((), None), (('--step', '0'), 0)]:
                with self.subTest(args=args):
                    result, _ = success(self, run, args, selector)
                    self.assertEqual(result['step_file'], raw)
                    assert_summary(self, result, STEP_SUMMARIES[0])
                    assert_fields(self, result, 0)

    def test_unselected_first_file_is_not_opened(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp)
            manifest = steps_fixture(run)
            (run / 'out/step-0.vtkhdf').unlink()
            # Preserve the first-file string guard but do not read its target.
            manifest['steps'][1]['file'] = 'out/./step-1.vtkhdf'
            save(run, manifest)
            result, _ = success(self, run, ('--step', '1'), 1)
            self.assertEqual(result['step_file'], 'out/./step-1.vtkhdf')
            assert_fields(self, result, 1)


if __name__ == '__main__':
    unittest.main(argv=[__file__])
