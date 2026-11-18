# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
import copy
import tempfile
import unittest
from pathlib import Path
from steps_fixtures import (fixture, steps_fixture, save, protocol_cli, api,
                            ResultSummaryError, success, assert_summary)


class Errors(unittest.TestCase):
    def assert_failure(self, run, code=14, args=(), message=None):
        actual, out, err = protocol_cli(run, *args)
        self.assertEqual(out, '')
        self.assertEqual(actual, code, err)
        name = 'INPUT_INVALID' if code == 14 else 'INPUT_REFERENCE_MISSING'
        self.assertTrue(err.startswith(f'result_summary: {name}: '), err)
        self.assertEqual(len(err.splitlines()), 1, err)
        if message is not None:
            self.assertIn(message, err)

    def assert_api_failure(self, run, selector, code=14, message=None):
        with self.assertRaises(ResultSummaryError) as caught:
            api(self, run, selector)
        self.assertEqual(caught.exception.code, code)
        self.assertEqual(caught.exception.name,
                         'INPUT_INVALID' if code == 14 else 'INPUT_REFERENCE_MISSING')
        if message is not None:
            self.assertIn(message, str(caught.exception))

    def test_invalid_cli_selectors(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp)
            fixture(run)
            for selector in ('LAST', 'Last', '-1', '1.5', '', '0x1',
                             '+1', ' 1', '1 ', '١', '１'):
                with self.subTest(selector=selector):
                    self.assert_failure(run, args=('--step', selector))

    def test_invalid_api_selectors(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp)
            fixture(run)
            for selector in (True, False, 1., -1, '0', 'LAST', 'Last',
                             '', '0x1', object(), [], {}):
                with self.subTest(selector=repr(selector)):
                    self.assert_api_failure(run, selector)

    def test_invalid_step_records_and_ids(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp)
            _, original = fixture(run)
            records = [None, [], 'step', 1, {},
                       {'file': 'out/step-0.vtkhdf'}]
            records += [dict(n=n, file='out/step-0.vtkhdf')
                        for n in (True, False, 1., '1', -1, None)]
            for position in (0, 1):
                for record in records:
                    with self.subTest(position=position, record=record):
                        manifest = copy.deepcopy(original)
                        if position == 0:
                            manifest['steps'][0] = record
                        else:
                            manifest['steps'].append(record)
                        save(run, manifest)
                        # The default must validate all IDs, including later rows.
                        self.assert_failure(run)

    def test_empty_steps(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp)
            _, manifest = fixture(run)
            manifest['steps'] = []
            save(run, manifest)
            self.assert_failure(run)

    def test_duplicate_step_ids_outside_selection(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp)
            _, manifest = fixture(run)
            manifest['steps'] += [dict(n=1, file='not-opened'),
                                  dict(n=1, file='also-not-opened')]
            save(run, manifest)
            self.assert_failure(run)

    def test_included_duplicate_summary_names(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp)
            _, manifest = fixture(run)
            original = copy.deepcopy(manifest)
            for owners in ((0, 0), (None, 0), (0, None)):
                with self.subTest(owners=owners):
                    manifest = copy.deepcopy(original)
                    manifest['summary'] = []
                    for owner in owners:
                        entry = dict(name='warpage_pv', value=.1, unit='m')
                        if owner is not None:
                            entry['step'] = owner
                        manifest['summary'].append(entry)
                    save(run, manifest)
                    self.assert_failure(run, message='duplicate summary name')

    def test_duplicate_names_excluded_from_selected_step(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp)
            _, manifest = fixture(run)
            manifest['summary'] += [dict(name='other', value=1, step=7),
                                    dict(name='other', value=2, step=7)]
            save(run, manifest)
            result, _ = success(self, run)
            assert_summary(self, result, [('T_max', 309.), ('warpage_pv', .1)])

    def test_invalid_summary_step_on_any_owner(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp)
            _, original = fixture(run)
            for value in (True, False, 1., '1', -1, None):
                with self.subTest(value=value):
                    manifest = copy.deepcopy(original)
                    # A tag such as 1.0 must be rejected even though it does not
                    # match selected n=0; malformed ownership cannot be filtered.
                    manifest['summary'].append(dict(name='extra', value=1, step=value))
                    save(run, manifest)
                    self.assert_failure(run)

    def test_excluded_summary_still_validates_shape_value_and_unit(self):
        malformed = [None, [], {}, dict(name='', value=1, step=7),
                     dict(name='missing_value', step=7),
                     dict(name=1, value=1, step=7)]
        malformed += [dict(name='bad_value', value=value, step=7)
                      for value in (None, True, '1', [], {})]
        malformed += [dict(name='warpage_pv', value=.2, unit=unit, step=7)
                      for unit in (None, '', 'mm')]
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp)
            _, original = fixture(run)
            for entry in malformed:
                with self.subTest(entry=entry):
                    manifest = copy.deepcopy(original)
                    # Remove legacy PV to avoid an unrelated duplicate-name error.
                    manifest['summary'] = [dict(name='good', value=1), entry]
                    save(run, manifest)
                    self.assert_failure(run)

    def test_nonfinite_summary_values_on_any_owner(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp)
            _, original = fixture(run)
            for owner in (None, 0, 7):
                for value in (float('nan'), float('inf'), -float('inf')):
                    with self.subTest(owner=owner, value=value):
                        manifest = copy.deepcopy(original)
                        entry = dict(name='nonfinite', value=value)
                        if owner is not None:
                            entry['step'] = owner
                        manifest['summary'].append(entry)
                        save(run, manifest)
                        self.assert_failure(run)

    def test_missing_requested_step(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp)
            steps_fixture(run)
            self.assert_failure(run, 12, ('--step', '7'), 'step 7')
            self.assert_api_failure(run, 7, 12, 'step 7')

    def test_selected_missing_and_unreadable_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp)
            manifest = steps_fixture(run)
            for file in ('out/missing.vtkhdf', 'out/broken.vtkhdf', 'out'):
                with self.subTest(file=file):
                    (run / 'out/broken.vtkhdf').write_text('not HDF5', encoding='utf-8')
                    manifest['steps'][1]['file'] = file
                    save(run, manifest)
                    self.assert_failure(run, 12, ('--step', '1'))

    def test_selected_file_must_be_nonempty_string(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp)
            manifest = steps_fixture(run)
            for file in (None, '', 1, True, []):
                with self.subTest(file=file):
                    manifest['steps'][1]['file'] = file
                    save(run, manifest)
                    self.assert_failure(run, args=('--step', '1'))

    def test_no_matching_summaries(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp)
            _, manifest = fixture(run)
            for entries in ([], [dict(name='warpage_pv', value=.4, unit='m', step=7)]):
                manifest['summary'] = entries
                save(run, manifest)
                for args, selector in [((), None), (('--step', '0'), 0),
                                       (('--step', 'last'), 'last')]:
                    with self.subTest(entries=entries, args=args):
                        result, _ = success(self, run, args, selector)
                        self.assertEqual(result['manifest_summary'], {})
                        self.assertIsNone(result['warpage']['manifest_warpage_pv_m'])
                        self.assertIsNone(result['warpage']['rel_diff'])


if __name__ == '__main__':
    unittest.main(argv=[__file__])
