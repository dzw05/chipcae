# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
import tempfile
import unittest
from pathlib import Path
from fixtures import fixture, fixed_keys, h5py, LAYERS, MATERIALS
from tools.vv.result_summary import SCHEMA, summarize


class HexOnly(unittest.TestCase):
    def test_duplicate_displacement_keeps_first(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp)
            step, _ = fixture(run)
            with h5py.File(step, 'r+') as f:
                block = f['VTKHDF/hex']
                self.assertEqual(block['Points'][5].tolist(), block['Points'][6].tolist())
                # The second partition repeats the center with a conflicting U.
                block['PointData/U'][6] = [100., 200., 300.]
            result = summarize(run)
            self.assertEqual(result['warpage']['n_top_points'], 9)
            self.assertAlmostEqual(result['warpage']['w_bfp_m'], .125, delta=.125e-12)
            self.assertAlmostEqual(result['warpage']['rel_diff'], .25, delta=.25e-12)

    def test_partitioned_hex(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp)
            fixture(run)
            r = summarize(run)
            fixed_keys(self, r)
            self.assertEqual(SCHEMA, r['schema'])
            self.assertEqual(r['step_file'], 'out/step-0.vtkhdf')
            self.assertEqual(r['mesh_sha256'], 'synthetic-sha256')
            self.assertEqual(r['blocks'], {'hex': {'points': 11, 'cells': 2, 'cell_kind_counts': {'0': 1, '1': 1}}, 'cut': {'points': 0, 'cells': 0, 'type_counts': {}}, 'eb_surface': {'points': 7, 'polygons': 3}})
            self.assertEqual(list(r['blocks']['hex']['cell_kind_counts']), ['0', '1'])
            self.assertEqual(r['layer_names'], LAYERS)
            self.assertEqual(r['material_names'], MATERIALS)
            # Shared center has the same temperature in both partitions.
            self.assertEqual(r['temperature']['min'], 300.)
            self.assertEqual(r['temperature']['max'], 309.)
            self.assertEqual(r['temperature']['top_min'], 301.)
            self.assertEqual(r['temperature']['top_max'], 309.)
            self.assertAlmostEqual(r['temperature']['top_mean'], 305., delta=305.e-14)
            self.assertEqual(r['warpage']['n_top_points'], 9)
            # Symmetry removes linear tilt; r^2 spans [0, 2].
            self.assertAlmostEqual(r['warpage']['w_bfp_m'], .125, delta=.125e-12)
            self.assertEqual(r['warpage']['manifest_warpage_pv_m'], .1)
            self.assertAlmostEqual(r['warpage']['rel_diff'], .25, delta=.25e-12)
            self.assertEqual(list(r['manifest_summary']), ['T_max', 'warpage_pv'])
            self.assertEqual(r['manifest_summary'], {'T_max': 309., 'warpage_pv': .1})


if __name__ == '__main__':
    unittest.main(argv=[__file__])
