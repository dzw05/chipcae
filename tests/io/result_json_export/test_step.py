# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
import json
import unittest

from export_fixtures import (ExportCase, TOP_KEYS, cli, protocol_cli,
                             steps_fixture)


class Step(ExportCase):
    def setUp(self):
        super().setUp()
        steps_fixture(self.run)

    def test_default_zero_one_and_last_copy_all_selected_qoi(self):
        for args, selector, n in [((), None, 0), (('--step', '0'), 0, 0),
                                  (('--step', '1'), 1, 1),
                                  (('--step', '0001'), 1, 1),
                                  (('--step', 'last'), 'last', 2)]:
            with self.subTest(args=args):
                completed = cli(self.run, *args)
                self.assertEqual(completed.returncode, 0, completed.stderr.decode())
                self.assertEqual(completed.stderr, b'')
                obj = json.loads(completed.stdout)
                self.assertEqual(list(obj), TOP_KEYS)
                self.assertEqual(obj['qoi']['step_file'], f'out/step-{n}.vtkhdf')
                self.assert_qoi(obj, step=selector)

    def test_top_tolerance_is_forwarded_to_reader(self):
        for tolerance in (0, 1):
            with self.subTest(tolerance=tolerance):
                completed = cli(self.run, '--step', '1', '--top-rel-tol', str(tolerance))
                self.assertEqual(completed.returncode, 0, completed.stderr.decode())
                self.assertEqual(completed.stderr, b'')
                obj = json.loads(completed.stdout)
                self.assert_qoi(obj, step=1, top_rel_tol=tolerance)

    def test_invalid_step_selectors_use_invalid_protocol(self):
        for selector in ('-1', '+1', '1.0', 'LAST', '', ' 1', '1 ', '١', '１'):
            with self.subTest(selector=selector):
                self.assert_failure(protocol_cli(self.run, '--step', selector), 'step')

    def test_missing_step_id_or_selected_file_uses_reference_missing_protocol(self):
        self.assert_failure(protocol_cli(self.run, '--step', '99'), 'step', code=12)
        (self.run / 'out/step-1.vtkhdf').unlink()
        self.assert_failure(protocol_cli(self.run, '--step', '1'), code=12)

    def test_invalid_top_tolerance_uses_invalid_protocol(self):
        for value in ('-1', 'nan', 'inf', '-inf', 'not-a-number'):
            with self.subTest(value=value):
                self.assert_failure(protocol_cli(
                    self.run, '--top-rel-tol=' + value), 'top_rel_tol')


if __name__ == '__main__':
    unittest.main(argv=[__file__])
