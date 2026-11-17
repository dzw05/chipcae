# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
import contextlib
import io
import tempfile
import unittest
from pathlib import Path
from fixtures import fixture, fixed_keys, save, h5py, np, LAYERS, MATERIALS
from tools.vv.result_summary import ResultSummaryError, main, summarize


class Optional(unittest.TestCase):
    def test_top_surface_tolerance(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp)
            step, _ = fixture(run)
            with h5py.File(step, 'r+') as f:
                block = f['VTKHDF/hex']
                points = block['Points'][...]
                # Lower both copies of the center by less than default tol * H.
                center = (points[:, 0] == 0.) & (points[:, 1] == 0.) & (points[:, 2] == 1.)
                self.assertEqual(int(np.count_nonzero(center)), 2)
                points[center, 2] = 1. - 5e-13
                block['Points'][...] = points
                temperatures = block['PointData/T'][...]
                temperatures[center] = 1000.
                block['PointData/T'][...] = temperatures
            default = summarize(run)
            exact = summarize(run, top_rel_tol=0.)
            for result in (default, exact):
                fixed_keys(self, result)
            self.assertEqual(default['warpage']['n_top_points'], 9)
            self.assertEqual(exact['warpage']['n_top_points'], 8)
            for result, maximum, mean, warpage in (
                    (default, 1000., 3440. / 9., .125),
                    (exact, 309., 305., .0625)):
                self.assertEqual(result['temperature']['top_min'], 301.)
                self.assertEqual(result['temperature']['top_max'], maximum)
                self.assertLessEqual(abs(result['temperature']['top_mean'] / mean - 1.), 1e-14)
                # Quadratic residual range is k/2 with center, k/4 without it.
                self.assertLessEqual(abs(result['warpage']['w_bfp_m'] / warpage - 1.), 1e-12)

    def test_single_name_table(self):
        for block in ('hex', 'cut'):
            with self.subTest(block=block), tempfile.TemporaryDirectory() as tmp:
                run = Path(tmp)
                step, _ = fixture(run)
                with h5py.File(step, 'r+') as f:
                    del f['VTKHDF/' + block + '/FieldData']
                result = summarize(run)
                fixed_keys(self, result)
                self.assertEqual(result['layer_names'], LAYERS)
                self.assertEqual(result['material_names'], MATERIALS)

    def test_malformed_optional_containers(self):
        for key, values, message in (
                ('mesh', (None, [], '', 1, True), 'mesh must be an object'),
                ('summary', (None, {}, '', 1, True), 'summary must be a list')):
            for value in values:
                with self.subTest(key=key, value=value), tempfile.TemporaryDirectory() as tmp:
                    run = Path(tmp)
                    _, manifest = fixture(run)
                    manifest[key] = value
                    save(run, manifest)
                    with self.assertRaises(ResultSummaryError) as caught:
                        summarize(run)
                    self.assertEqual(caught.exception.code, 14)
                    self.assertEqual(caught.exception.name, 'INPUT_INVALID')
                    self.assertEqual(str(caught.exception), message)
                    err, out = io.StringIO(), io.StringIO()
                    with contextlib.redirect_stderr(err), contextlib.redirect_stdout(out):
                        self.assertEqual(main([str(run)]), 14)
                    self.assertEqual(out.getvalue(), '')
                    self.assertEqual(err.getvalue(), f'result_summary: INPUT_INVALID: {message}\n')

    def test_optional_fields(self):
        for case in ('T', 'eb_surface', 'warpage_pv', 'two_points', 'names', 'mesh', 'zero_pv', 'absolute_step'):
            with self.subTest(case=case), tempfile.TemporaryDirectory() as tmp:
                run = Path(tmp) / 'run'
                step, manifest = fixture(run, two=case == 'two_points')
                with h5py.File(step, 'r+') as f:
                    if case == 'T':
                        for block in ('hex', 'cut'):
                            del f['VTKHDF/'+block+'/PointData/T']
                    elif case == 'eb_surface':
                        del f['VTKHDF/eb_surface']
                    elif case == 'names':
                        for block in ('hex', 'cut'):
                            del f['VTKHDF/'+block+'/FieldData']
                if case == 'warpage_pv':
                    manifest['summary'] = manifest['summary'][:1]
                elif case == 'mesh':
                    del manifest['mesh']
                elif case == 'zero_pv':
                    manifest['summary'][1]['value'] = 0.
                elif case == 'absolute_step':
                    original_step = step
                    relative_file = manifest['steps'][0]['file']
                    step = step.rename(Path(tmp) / 'external-step.vtkhdf')
                    self.assertFalse(original_step.exists())
                    manifest['steps'][0]['file'] = str(step)
                save(run, manifest)
                r = summarize(run)
                fixed_keys(self, r)
                if case == 'T':
                    self.assertIsNone(r['temperature'])
                elif case == 'eb_surface':
                    self.assertEqual(r['blocks']['eb_surface'], {'points': 0, 'polygons': 0})
                elif case == 'warpage_pv':
                    self.assertIsNone(r['warpage']['manifest_warpage_pv_m'])
                    self.assertIsNone(r['warpage']['rel_diff'])
                elif case == 'two_points':
                    self.assertEqual(r['warpage']['n_top_points'], 2)
                    self.assertEqual(r['temperature']['top_min'], 304.)
                    self.assertEqual(r['temperature']['top_max'], 306.)
                    self.assertEqual(r['temperature']['top_mean'], 305.)
                    self.assertIsNone(r['warpage']['w_bfp_m'])
                    self.assertIsNone(r['warpage']['rel_diff'])
                elif case == 'names':
                    self.assertEqual(r['layer_names'], [])
                    self.assertEqual(r['material_names'], [])
                elif case == 'mesh':
                    self.assertIsNone(r['mesh_sha256'])
                elif case == 'zero_pv':
                    self.assertEqual(r['warpage']['manifest_warpage_pv_m'], 0.)
                    self.assertIsNone(r['warpage']['rel_diff'])
                else:
                    self.assertEqual(r['step_file'], str(step))
                    self.assertEqual(r['warpage']['n_top_points'], 9)
                    self.assertLessEqual(abs(r['warpage']['w_bfp_m'] / .125 - 1.), 1e-12)
                    manifest['steps'][0]['file'] = relative_file
                    save(run, manifest)
                    with self.assertRaises(ResultSummaryError) as caught:
                        summarize(run)
                    self.assertEqual(caught.exception.code, 12)
                    self.assertEqual(caught.exception.name, 'INPUT_REFERENCE_MISSING')


if __name__ == '__main__':
    unittest.main(argv=[__file__])
