# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
import copy
import os
from pathlib import Path
import unittest
from unittest.mock import patch

from export_fixtures import (ExportCase, QOI_KEYS, cli, encoded, exporter,
                             protocol_cli, summarize)


class Errors(ExportCase):
    def test_metadata_cli_fullmatch_and_strict_enums(self):
        invalid = {
            'data_class': ['Synthetic', 'synthetic ', '', 'unknown'],
            'label': ['实测 ', 'measured', ''],
            'verdict': ['PASS', 'pass ', '', 'unknown'],
            'case_id': ['bad id', '', '.hidden', '_leading', 'a\n', '中文'],
            'date': ['2026-11-18', '2026111', '202611180', '２０２６１１１８',
                     '20261118\n', ''],
            'commit': ['', 'abcdef', 'a' * 41, 'ABCDEF1', 'ggggggg',
                       'abcdef1\n', ' abcdef1'],
        }
        for field, values in invalid.items():
            for value in values:
                with self.subTest(field=field, value=value):
                    self.assert_failure(protocol_cli(self.run, **{field: value}), field)

    def test_reference_requires_all_three_cli_flags(self):
        flags = [('reference-value', '.125'), ('reference-type', '解析'),
                 ('reference-source', 'synthetic reference')]
        for mask in range(1, 7):
            args = [part for i, (flag, value) in enumerate(flags)
                    if mask & (1 << i) for part in ('--' + flag, value)]
            with self.subTest(mask=mask):
                self.assert_failure(protocol_cli(self.run, *args), 'reference')

    def test_invalid_reference_type_and_empty_source(self):
        for kind, source, field in [('解析 ', 'synthetic', 'reference_type'),
                                     ('analytical', 'synthetic', 'reference_type'),
                                     ('解析', '', 'reference_source')]:
            with self.subTest(kind=kind, source=source):
                self.assert_failure(protocol_cli(
                    self.run, '--reference-value', '1', '--reference-type', kind,
                    '--reference-source', source), field)

    def test_metrics_without_value_are_invalid(self):
        for field in ('error', 'tolerance'):
            with self.subTest(field=field):
                self.assert_failure(protocol_cli(
                    self.run, '--' + field + '-metric', 'rel'), field)

    def test_all_cli_numbers_reject_nonfinite_or_nonnumeric_values(self):
        for field in ('reference', 'error', 'tolerance'):
            for value in ('nan', 'inf', '-inf', '1e999', 'not-a-number'):
                with self.subTest(field=field, value=value):
                    args = [f'--{field}-value={value}']
                    if field == 'reference':
                        args.extend(('--reference-type', '解析',
                                     '--reference-source', 'synthetic'))
                    self.assert_failure(protocol_cli(self.run, *args), field)

    def test_api_metadata_and_optional_field_validation(self):
        invalid = [
            ('case_id', None), ('case_id', 'a\n'), ('data_class', 'Synthetic'),
            ('label', '实测 '), ('date', 20261118), ('date', '20261118\n'),
            ('commit', 'ABCDEF1'), ('verdict', 'PASS'),
            ('notes', 'a'), ('notes', ['ok', 1]),
            ('src_dirty', 0), ('src_dirty', 1), ('src_dirty', 'false'),
            ('reference', []), ('reference', {}),
            ('reference', {'value': 1, 'source': 'synthetic'}),
            ('reference', {'value': 1, 'type': '解析'}),
            ('reference', {'type': '解析', 'source': 'synthetic'}),
            ('reference', {'value': 1, 'type': '解析 ', 'source': 'synthetic'}),
            ('reference', {'value': 1, 'type': '解析', 'source': ''}),
            ('reference', {'value': 1, 'type': '解析', 'source': 7}),
            ('error', []), ('error', {}), ('error', {'metric': 'rel'}),
            ('error', {'value': 1, 'metric': None}),
            ('tolerance', []), ('tolerance', {}),
            ('tolerance', {'metric': 'rel'}),
            ('tolerance', {'value': 1, 'metric': 7}),
        ]
        for field, value in invalid:
            with self.subTest(field=field, value=value):
                self.assert_api_invalid(field, **{field: value})

    def test_api_numbers_reject_bool_nan_inf_and_strings(self):
        for field in ('reference', 'error', 'tolerance'):
            for value in (True, False, float('nan'), float('inf'), -float('inf'), '1'):
                with self.subTest(field=field, value=value):
                    option = {'value': value}
                    if field == 'reference':
                        option.update(type='解析', source='synthetic')
                    self.assert_api_invalid(field, **{field: option})

    def test_constructor_rejects_nested_nonfinite_qoi_values(self):
        summary = summarize(self.run)
        for field in ('blocks', 'temperature', 'warpage', 'manifest_summary'):
            for sequence in (list, tuple):
                for value in (float('nan'), float('inf'), -float('inf')):
                    with self.subTest(field=field, sequence=sequence, value=value):
                        invalid = copy.deepcopy(summary)
                        invalid[field]['extra'] = {'samples': sequence([{'value': value}])}
                        with self.assertRaises(exporter.ResultJsonError) as caught:
                            self.object(invalid)
                        self.assertEqual(caught.exception.code, 14)
                        self.assertEqual(caught.exception.name, 'INPUT_INVALID')
                        self.assertIn(f'qoi.{field}', str(caught.exception))

    def test_constructor_rejects_nested_nonfinite_optional_extras(self):
        for field in ('reference', 'error', 'tolerance'):
            for sequence in (list, tuple):
                for value in (float('nan'), float('inf'), -float('inf')):
                    with self.subTest(field=field, sequence=sequence, value=value):
                        option = {'value': 1, 'extra': {'samples': sequence([{'value': value}])}}
                        if field == 'reference':
                            option.update(type='解析', source='synthetic')
                        self.assert_api_invalid(field, **{field: option})

    def test_missing_qoi_key_reports_field_as_protocol_error(self):
        summary = summarize(self.run)
        for key in QOI_KEYS:
            with self.subTest(key=key):
                missing = copy.deepcopy(summary)
                del missing[key]
                with self.assertRaises(exporter.ResultJsonError) as caught:
                    exporter.qoi_from_summary(missing)
                self.assertEqual(caught.exception.code, 14)
                self.assertEqual(caught.exception.name, 'INPUT_INVALID')
                self.assertIn(key, str(caught.exception))

    def test_missing_manifest_and_missing_directory_preserve_reader_code(self):
        (self.run / 'manifest.json').unlink()
        for run in (self.run, self.base / 'missing\nrun'):
            with self.subTest(run=run):
                self.assert_failure(protocol_cli(run), code=12)
        completed = cli(self.run)
        self.assert_failure((completed.returncode, completed.stdout.decode(),
                             completed.stderr.decode()), code=12)

    def test_corrupt_manifest_preserves_invalid_code(self):
        (self.run / 'manifest.json').write_text('{\nnot JSON', encoding='utf-8')
        self.assert_failure(protocol_cli(self.run), 'manifest', code=14)

    def test_output_directory_and_missing_parent_fail_without_creating_parents(self):
        for target in (self.run, self.base / 'absent' / 'result.json'):
            with self.subTest(target=target):
                self.assert_failure(protocol_cli(self.run, '-o', str(target)))
        self.assertFalse((self.base / 'absent').exists())

    def test_replace_failure_preserves_old_file_and_removes_temporary(self):
        old = b'previous result remains byte-for-byte intact\n'
        self.target.write_bytes(old)
        before = set(self.base.iterdir())
        expected = encoded(self.object())
        replacements = []

        def fail_replace(source, destination, *args, **kwargs):
            source, destination = Path(source), Path(destination)
            replacements.append((source, destination))
            self.assertEqual(source.parent.resolve(), self.target.parent.resolve())
            self.assertNotEqual(source.resolve(), self.target.resolve())
            self.assertEqual(destination.resolve(), self.target.resolve())
            self.assertEqual(source.read_bytes(), expected)
            raise OSError('controlled replace failure\nsecond line')

        with patch.object(os, 'replace', side_effect=fail_replace):
            result = protocol_cli(self.run, '-o', str(self.target))
        self.assert_failure(result)
        self.assertEqual(len(replacements), 1)
        self.assertEqual(self.target.read_bytes(), old)
        self.assertEqual(set(self.base.iterdir()), before)

    def test_write_api_io_and_serialization_failures_are_protocol_errors(self):
        self.target.write_bytes(b'keep old content')
        before = set(self.base.iterdir())
        for obj in ({'bad': float('nan')}, {'bad': float('inf')}, {'bad': {1, 2}}):
            with self.subTest(obj=obj):
                with self.assertRaises(exporter.ResultJsonError) as caught:
                    exporter.write_result_json(self.target, obj)
                self.assertEqual(caught.exception.code, 14)
                self.assertEqual(caught.exception.name, 'INPUT_INVALID')
                self.assertEqual(self.target.read_bytes(), b'keep old content')
                self.assertEqual(set(self.base.iterdir()), before)
        with self.assertRaises(exporter.ResultJsonError) as caught:
            exporter.write_result_json(self.run, self.object())
        self.assertEqual(caught.exception.code, 14)
        self.assertEqual(caught.exception.name, 'INPUT_INVALID')

    def test_parser_errors_use_one_line_protocol_and_help_succeeds(self):
        for args in (('--unknown',), ('--error-value',), ('--step',)):
            with self.subTest(args=args):
                self.assert_failure(protocol_cli(self.run, *args))
        completed = cli(self.run, '--help')
        self.assertEqual(completed.returncode, 0, completed.stderr.decode())
        self.assertEqual(completed.stderr, b'')
        self.assertIn(b'--case-id', completed.stdout)


if __name__ == '__main__':
    unittest.main(argv=[__file__])
