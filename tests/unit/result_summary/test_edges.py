# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
"""Additional coverage for structural validation and documented edge policies."""
import tempfile
import unittest
from pathlib import Path
from fixtures import fixture, h5py, np
from tools.vv.result_summary import ResultSummaryError, summarize


class Edges(unittest.TestCase):
    def test_partial_temperature_and_byte_order(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp)
            step, _ = fixture(run, cut=True)
            with h5py.File(step, 'r+') as f:
                del f['VTKHDF/cut/PointData/T']
                block = f['VTKHDF/hex']
                u = block['PointData/U'][...].astype('>f8')
                del block['PointData/U']
                block['PointData/U'] = u
            result = summarize(run)
            self.assertEqual(result['temperature']['min'], 300.)
            self.assertEqual(result['temperature']['top_mean'], 305.)
            self.assertEqual(result['warpage']['n_top_points'], 10)

    def test_empty_geometry(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp)
            step, _ = fixture(run)
            with h5py.File(step, 'r+') as f:
                b = f['VTKHDF/hex']
                for path in ('Points', 'PointData', 'CellData', 'Types'):
                    del b[path]
                b['NumberOfPoints'][...] = 0
                b['NumberOfCells'][...] = 0
            result = summarize(run)
            self.assertIsNone(result['temperature'])
            self.assertEqual(result['warpage']['n_top_points'], 0)
            self.assertIsNone(result['warpage']['w_bfp_m'])

    def test_structural_errors(self):
        for case in ('corrupt', 'U_components', 'negative_counts', 'cut_types', 'root_version'):
            with self.subTest(case=case), tempfile.TemporaryDirectory() as tmp:
                run = Path(tmp)
                step, _ = fixture(run, cut=True)
                if case == 'corrupt':
                    step.write_bytes(b'not HDF5')
                else:
                    with h5py.File(step, 'r+') as f:
                        if case == 'U_components':
                            del f['VTKHDF/hex/PointData/U']
                            f['VTKHDF/hex/PointData/U'] = np.zeros((11, 2))
                        elif case == 'negative_counts':
                            f['VTKHDF/hex/NumberOfPoints'][0] = -1
                        elif case == 'cut_types':
                            del f['VTKHDF/cut/Types']
                            f['VTKHDF/cut/Types'] = np.array([42], dtype='uint8')
                        else:
                            f['VTKHDF'].attrs['Version'] = [2, 7]
                with self.assertRaises(ResultSummaryError) as caught:
                    summarize(run)
                self.assertEqual(caught.exception.code, 14)


if __name__ == '__main__':
    unittest.main(argv=[__file__])
