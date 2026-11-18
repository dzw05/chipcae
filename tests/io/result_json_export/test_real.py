# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
import json
import unittest

from export_fixtures import ExportCase, TOP_KEYS, check_file, cli, protocol_cli


WARN = b'result_summary_result: WARN: data_class=real must stay local (D03)\n'


class Real(ExportCase):
    def test_real_file_is_written_and_only_d03_is_reported(self):
        completed = cli(self.run, '-o', str(self.target), data_class='real')
        self.assertEqual(completed.returncode, 0, completed.stderr.decode())
        self.assertEqual(completed.stdout, b'')
        self.assertEqual(completed.stderr, WARN)
        self.assertTrue(self.target.is_file())
        self.assertEqual(check_file(self.target), ['real result must stay local (D03)'])
        obj = json.loads(self.target.read_bytes())
        self.assertEqual(list(obj), TOP_KEYS)
        self.assertEqual(obj['data_class'], 'real')
        self.assertEqual(obj['label'], '实测')
        self.assert_qoi(obj)

    def test_real_stdout_has_json_and_exact_warning(self):
        completed = cli(self.run, data_class='real')
        self.assertEqual(completed.returncode, 0, completed.stderr.decode())
        self.assertEqual(completed.stderr, WARN)
        self.assertEqual(json.loads(completed.stdout)['data_class'], 'real')

    def test_real_failed_write_reports_error_without_success_warning(self):
        result = protocol_cli(self.run, '-o', str(self.run), data_class='real')
        self.assert_failure(result)
        self.assertNotIn('WARN', result[2])


if __name__ == '__main__':
    unittest.main(argv=[__file__])
