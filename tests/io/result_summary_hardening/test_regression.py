# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
import contextlib
import io
import math
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
from hardening_fixtures import (
    fixture, save, set_pv, h5py, rs, success, cli, invalid_cli, invalid_api,
    overflow_displacement, overflow_temperature,
)


class Regression(unittest.TestCase):
    def test_normal_fixture_keeps_fixed_keys_and_json_bytes(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp) / 'run'
            fixture(run)
            result = success(self, run)
            self.assertEqual(result['mesh_sha256'], 'synthetic-sha256')
            self.assertEqual(result['warpage']['n_top_points'], 9)
            # Nine symmetric nodes have r^2 in [0, 2], giving PV = 2/16 m.
            self.assertTrue(math.isclose(result['warpage']['w_bfp_m'], .125,
                                         rel_tol=1e-12, abs_tol=1e-14))
            self.assertTrue(math.isclose(result['temperature']['top_mean'], 305.,
                                         rel_tol=1e-12, abs_tol=1e-14))

    def test_huge_signed_integer_pv_stays_integer_with_finite_float_ratio(self):
        for pv in (10**400, -(10**400)):
            with self.subTest(sign=pv > 0), tempfile.TemporaryDirectory() as tmp:
                run = Path(tmp) / 'run'
                _, manifest = fixture(run)
                set_pv(run, manifest, pv)
                result = success(self, run)
                for value in (result['warpage']['manifest_warpage_pv_m'],
                              result['manifest_summary']['warpage_pv']):
                    self.assertIs(type(value), int)
                    self.assertEqual(value, pv)
                ratio = result['warpage']['rel_diff']
                self.assertIs(type(ratio), float)
                self.assertTrue(math.isfinite(ratio))
                self.assertTrue(math.isclose(ratio, 1. if pv > 0 else -1.,
                                             rel_tol=1e-12, abs_tol=1e-14))

    def test_zero_and_missing_pv_keep_null_ratio(self):
        for missing in (False, True):
            with self.subTest(missing=missing), tempfile.TemporaryDirectory() as tmp:
                run = Path(tmp) / 'run'
                _, manifest = fixture(run)
                if missing:
                    manifest['summary'] = [e for e in manifest['summary']
                                           if e['name'] != 'warpage_pv']
                    save(run, manifest)
                else:
                    set_pv(run, manifest, 0)
                self.assertIsNone(success(self, run)['warpage']['rel_diff'])

    def test_less_than_three_top_points_keeps_null_warpage(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp) / 'run'
            fixture(run, two=True)
            warpage = success(self, run)['warpage']
            self.assertEqual(warpage['n_top_points'], 2)
            self.assertIsNone(warpage['w_bfp_m'])
            self.assertIsNone(warpage['rel_diff'])

    def test_absent_eb_surface_keeps_zero_counts(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp) / 'run'
            step, _ = fixture(run)
            with h5py.File(step, 'r+') as f:
                del f['VTKHDF/eb_surface']
            self.assertEqual(success(self, run)['blocks']['eb_surface'],
                             {'points': 0, 'polygons': 0})

    def test_finite_temperatures_with_overflowing_mean_report_temperature(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp) / 'run'
            step, _ = fixture(run)
            overflow_temperature(step)
            with self.subTest(interface='CLI'):
                invalid_cli(self, cli(run), 'temperature.top_mean', 'non-finite')
            with self.subTest(interface='API'):
                invalid_api(self, run, 'temperature.top_mean', 'non-finite')

    def test_temperature_precedes_both_nonfinite_warpage_fields(self):
        for pv in (.1, 1):
            with self.subTest(pv=pv), tempfile.TemporaryDirectory() as tmp:
                run = Path(tmp) / 'run'
                step, manifest = fixture(run)
                set_pv(run, manifest, pv)
                overflow_displacement(step)
                overflow_temperature(step)
                with self.subTest(interface='CLI'):
                    completed = cli(run)
                    invalid_cli(self, completed, 'temperature.top_mean', 'non-finite')
                    self.assertNotIn(b'warpage.w_bfp_m', completed.stderr)
                    self.assertNotIn(b'warpage.rel_diff', completed.stderr)
                with self.subTest(interface='API'):
                    message = invalid_api(self, run, 'temperature.top_mean', 'non-finite')
                    self.assertNotIn('warpage.w_bfp_m', message)
                    self.assertNotIn('warpage.rel_diff', message)

    def test_complete_result_guard_traverses_nested_dicts_and_lists_in_order(self):
        # Nested arbitrary output cannot arise from the current HDF5 schema.
        # Inject at the read boundary to exercise the final result validation,
        # without requiring a particular new private validator name/signature.
        read = rs._read
        cases = [
            ({'items': [0, {'value': value}]}, 'blocks.probe.items[1].value')
            for value in (float('nan'), float('inf'), float('-inf'))
        ] + [
            ({'z_first': {'deep': float('nan')}, 'a_second': float('inf')},
             'blocks.probe.z_first.deep'),
            ({'items': [{'deep': float('-inf')}, float('nan')]},
             'blocks.probe.items[0].deep'),
        ]
        for nested, path in cases:
            with self.subTest(path=path, nested=nested), tempfile.TemporaryDirectory() as tmp:
                run = Path(tmp) / 'run'
                step, _ = fixture(run)
                overflow_temperature(step)

                def injected(root):
                    blocks, data, temperatures, names = read(root)
                    blocks['probe'] = nested
                    return blocks, data, temperatures, names

                with patch.object(rs, '_read', side_effect=injected):
                    message = invalid_api(self, run, path, 'non-finite')
                self.assertNotIn('temperature.top_mean', message)
                self.assertNotIn('a_second', message)

    def test_serialization_value_error_is_a_single_protocol_line(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp) / 'run'
            fixture(run)
            out, err = io.StringIO(), io.StringIO()
            # Valid input exercises real summarization; only the defensive
            # serializer failure is simulated because the guard should prevent it.
            with patch.object(rs.json, 'dumps', side_effect=ValueError('first\nsecond')):
                with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                    try:
                        code = rs.main([str(run)])
                    except ValueError as exc:
                        self.fail(f'main leaked serialization ValueError: {exc!r}')
            self.assertEqual(code, 14)
            self.assertEqual(out.getvalue(), '')
            self.assertEqual(err.getvalue(),
                             'result_summary: INPUT_INVALID: first second\n')


if __name__ == '__main__':
    unittest.main(argv=[__file__])
