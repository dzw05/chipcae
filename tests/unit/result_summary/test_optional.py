# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
import tempfile
import unittest
from pathlib import Path
from fixtures import fixture, fixed_keys, save, h5py, LAYERS, MATERIALS
from tools.vv.result_summary import summarize


class Optional(unittest.TestCase):
    def test_single_name_table(self):
        for block in ('hex', 'cut'):
            with self.subTest(block=block), tempfile.TemporaryDirectory() as tmp:
                run = Path(tmp)
                step, _ = fixture(run)
                with h5py.File(step, 'r+') as f:
                    del f['VTKHDF/' + block + '/FieldData']
                result = summarize(run)
                self.assertEqual(result['layer_names'], LAYERS)
                self.assertEqual(result['material_names'], MATERIALS)

    def test_optional_fields(self):
        for case in ('T', 'eb_surface', 'warpage_pv', 'two_points', 'names', 'mesh', 'zero_pv', 'absolute_step'):
            with self.subTest(case=case), tempfile.TemporaryDirectory() as tmp:
                run = Path(tmp)
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


if __name__ == '__main__':
    unittest.main(argv=[__file__])
