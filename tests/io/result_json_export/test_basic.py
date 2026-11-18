# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
import copy
import json
import unittest

from export_fixtures import (ExportCase, METADATA, QOI_KEYS, TOP_KEYS, check_file,
                             cli, encoded, exporter, h5py, save, summarize)


class Basic(ExportCase):
    def test_cli_complete_metadata_order_and_repeatable_utf8(self):
        extra = ('--reference-value', '0.125', '--reference-type', '解析',
                 '--reference-source', '人造解析基准',
                 '--error-value', '0.01', '--error-metric', 'abs',
                 '--tolerance-value', '0.02', '--tolerance-metric', 'rel',
                 '--note', '重复备注', '--note', '重复备注', '--src-dirty')
        for data_class in ('synthetic', 'public'):
            with self.subTest(data_class=data_class):
                first = cli(self.run, '-o', str(self.target), *extra,
                            data_class=data_class)
                self.assertEqual(first.returncode, 0, first.stderr.decode())
                self.assertEqual(first.stdout, b'')
                self.assertEqual(first.stderr, b'')
                self.assertEqual(check_file(self.target), [])
                raw = self.target.read_bytes()
                obj = json.loads(raw)
                self.assertEqual(list(obj), TOP_KEYS + [
                    'reference', 'error', 'tolerance', 'notes', 'src_dirty'])
                for key, value in dict(METADATA, data_class=data_class).items():
                    self.assertEqual(obj[key], value)
                self.assertEqual(obj['schema'], 'chipcae.vv_result/0.2')
                self.assert_qoi(obj)
                self.assertEqual(obj['reference'], {
                    'value': .125, 'type': '解析', 'source': '人造解析基准'})
                self.assertEqual(obj['error'], {'value': .01, 'metric': 'abs'})
                self.assertEqual(obj['tolerance'], {'value': .02, 'metric': 'rel'})
                self.assertEqual(obj['notes'], ['重复备注', '重复备注'])
                self.assertIs(obj['src_dirty'], True)
                self.assertEqual(raw, encoded(obj))
                self.assertIn('实测'.encode('utf-8'), raw)
                second = cli(self.run, '-o', str(self.target), *extra,
                             data_class=data_class)
                self.assertEqual(second.returncode, 0, second.stderr.decode())
                self.assertEqual(second.stdout, b'')
                self.assertEqual(second.stderr, b'')
                self.assertEqual(self.target.read_bytes(), raw)
                stdout = cli(self.run, *extra, data_class=data_class)
                self.assertEqual(stdout.returncode, 0, stdout.stderr.decode())
                self.assertEqual(stdout.stderr, b'')
                self.assertEqual(stdout.stdout, raw)

    def test_api_omits_empty_options_and_keeps_false(self):
        obj = self.object(reference=None, error=None, tolerance=None,
                          notes=[], src_dirty=None)
        self.assertEqual(list(obj), TOP_KEYS)
        self.assertEqual(list(self.object()), TOP_KEYS)
        obj = self.object(src_dirty=False, notes=['a', 'a'])
        self.assertEqual(list(obj), TOP_KEYS + ['notes', 'src_dirty'])
        self.assertIs(obj['src_dirty'], False)
        self.assertEqual(obj['notes'], ['a', 'a'])
        self.assertEqual(obj['schema'], exporter.SCHEMA)
        self.assert_qoi(obj)

    def test_api_preserves_optional_values_without_computing_verdict(self):
        for value in (None, 0, 10**100, .125):
            with self.subTest(value=value):
                reference = dict(value=value, type='独立代码', source='synthetic')
                error = dict(value=value, metric='caller metric')
                tolerance = dict(value=value)
                obj = self.object(reference=reference, error=error,
                                  tolerance=tolerance, verdict='需讨论')
                self.assertEqual(list(obj), TOP_KEYS + [
                    'reference', 'error', 'tolerance'])
                self.assertEqual(obj['reference'], reference)
                self.assertEqual(obj['error'], error)
                self.assertEqual(obj['tolerance'], tolerance)
                for field in ('reference', 'error', 'tolerance'):
                    self.assertIs(type(obj[field]['value']), type(value))
                self.assertEqual(obj['verdict'], '需讨论')

    def test_qoi_copies_only_eight_keys_and_keeps_null(self):
        # Input insertion order must not determine the frozen QoI order.
        summary = dict(reversed(list(summarize(self.run).items())))
        summary['temperature'] = None
        summary['mesh_sha256'] = None
        summary['timing'] = {'seconds': 5}
        summary['future_summary_key'] = 'must not leak'
        original = copy.deepcopy(summary)
        qoi = exporter.qoi_from_summary(summary)
        self.assertEqual(list(qoi), QOI_KEYS)
        for key in QOI_KEYS:
            self.assertEqual(qoi[key], summary[key], key)
        self.assertIsNone(qoi['temperature'])
        self.assertIsNone(qoi['mesh_sha256'])
        obj = self.object(summary)
        self.assertEqual(list(obj), TOP_KEYS)
        self.assertEqual(obj['qoi'], qoi)
        self.assertEqual(summary, original)

    def test_nulls_from_real_reader_survive_export(self):
        with h5py.File(self.step, 'r+') as data:
            for name in ('hex', 'cut'):
                del data[f'VTKHDF/{name}/PointData/T']
        self.manifest.pop('mesh')
        save(self.run, self.manifest)
        completed = cli(self.run, '-o', str(self.target))
        self.assertEqual(completed.returncode, 0, completed.stderr.decode())
        self.assertEqual(completed.stderr, b'')
        obj = json.loads(self.target.read_bytes())
        self.assert_qoi(obj)
        self.assertIsNone(obj['qoi']['temperature'])
        self.assertIsNone(obj['qoi']['mesh_sha256'])
        self.assertEqual(check_file(self.target), [])

    def test_write_api_replaces_existing_file_with_exact_encoding(self):
        obj = self.object(notes=['中文'])
        self.target.write_bytes(b'old content that must be replaced')
        before = set(self.base.iterdir())
        exporter.write_result_json(self.target, obj)
        self.assertEqual(self.target.read_bytes(), encoded(obj))
        self.assertEqual(check_file(self.target), [])
        self.assertEqual(set(self.base.iterdir()), before)

    def test_error_and_tolerance_value_without_metric_are_valid(self):
        completed = cli(self.run, '--error-value', '0.01',
                        '--tolerance-value', '0')
        self.assertEqual(completed.returncode, 0, completed.stderr.decode())
        self.assertEqual(completed.stderr, b'')
        obj = json.loads(completed.stdout)
        self.assertEqual(list(obj), TOP_KEYS + ['error', 'tolerance'])
        self.assertEqual(obj['error'], {'value': .01})
        self.assertEqual(obj['tolerance'], {'value': 0})


if __name__ == '__main__':
    unittest.main(argv=[__file__])
