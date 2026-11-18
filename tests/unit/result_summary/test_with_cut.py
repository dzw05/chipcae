# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
import tempfile
import unittest
from pathlib import Path
from fixtures import fixture, fixed_keys, h5py
from tools.vv.result_summary import summarize


class WithCut(unittest.TestCase):
    def test_matching_duplicate_temperature(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp)
            step, _ = fixture(run, cut=True)
            with h5py.File(step, 'r+') as f:
                # Duplicate an off-center top point, whose T differs from the
                # mean: counting it twice would change the expected mean.
                for field in ('Points', 'PointData/U', 'PointData/T'):
                    f['VTKHDF/cut/' + field][0] = f['VTKHDF/hex/' + field][1]
            result = summarize(run)
            fixed_keys(self, result)
            self.assertEqual(result['warpage']['n_top_points'], 9)
            self.assertEqual(result['temperature']['min'], 100.)
            self.assertEqual(result['temperature']['max'], 309.)
            self.assertEqual(result['temperature']['top_min'], 301.)
            self.assertEqual(result['temperature']['top_max'], 309.)
            self.assertLessEqual(abs(result['temperature']['top_mean'] / 305. - 1.), 1e-14)

    def test_cut_type_counts_valid_domain(self):
        # The writer emits only VTK_POLYHEDRON (42) in cut blocks.
        # Empty and observed-only mappings cover its entire valid key domain.
        for cells in (0, 1, 2):
            with self.subTest(cells=cells), tempfile.TemporaryDirectory() as tmp:
                run = Path(tmp)
                step, _ = fixture(run, cut=True)
                with h5py.File(step, 'r+') as f:
                    cut = f['VTKHDF/cut']
                    cut['NumberOfCells'][...] = [min(cells, 1), max(cells - 1, 0)]
                    for field in ('Types', *('CellData/' + name for name in cut['CellData'])):
                        values = cut[field][...][:cells]
                        del cut[field]
                        cut[field] = values
                counts = summarize(run)['blocks']['cut']['type_counts']
                self.assertEqual(counts, {'42': cells} if cells else {})
                self.assertEqual(list(counts), ['42'] if cells else [])

    def test_cut_contributes(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp)
            fixture(run, cut=True)
            r = summarize(run)
            fixed_keys(self, r)
            self.assertEqual(r['blocks']['cut'], {'points': 2, 'cells': 2, 'type_counts': {'42': 2}})
            self.assertEqual(r['temperature']['min'], 100.)
            self.assertEqual(r['temperature']['max'], 999.)
            self.assertEqual(r['temperature']['top_min'], 301.)
            self.assertEqual(r['temperature']['top_max'], 999.)
            self.assertAlmostEqual(r['temperature']['top_mean'], (305.*9+999)/10, delta=374.4e-14)
            self.assertEqual(r['warpage']['n_top_points'], 10)


if __name__ == '__main__':
    unittest.main(argv=[__file__])
