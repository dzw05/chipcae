# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
from pathlib import Path
import tempfile
import unittest
from polyhedra_fixtures import CUBE, cube_fixture, h5py, invalid, np, replace, success


class Volume(unittest.TestCase):
    def test_signed_volume_rejects_reversed_collapsed_and_overflow_geometry(self):
        for fault in ('reverse', 'collapse', 'overflow'):
            with self.subTest(fault=fault):
                with tempfile.TemporaryDirectory() as tmp:
                    run = Path(tmp)
                    step, _ = cube_fixture(self, run)
                    with h5py.File(step, 'r+') as f:
                        b = f['VTKHDF/cut']
                        if fault == 'reverse':
                            b['FaceConnectivity'][...] = b['FaceConnectivity'][...].reshape(6, 4)[:, ::-1].ravel()
                        elif fault == 'collapse':
                            b['Points'][:, 2] = 0.
                        else:
                            b['Points'][...] = CUBE * 1e103
                        # The geometric rejection must work independently of stored volume.
                        del b['CellData/volume']
                    invalid(self, run, 'PolyhedronToFaces', 'nonpositive polyhedron volume',
                            'partition 0', 'cell 0', subprocess_cli=(fault == 'reverse'))

    def test_volume_checks_advance_point_and_cell_cursors(self):
        for partition, cell, first in [(0, 1, 8), (1, 0, 16)]:
            with self.subTest(partition=partition, cell=cell):
                with tempfile.TemporaryDirectory() as tmp:
                    run = Path(tmp)
                    step, _ = cube_fixture(self, run, cells=(2, 1))
                    with h5py.File(step, 'r+') as f:
                        f['VTKHDF/cut/Points'][first:first + 8, 2] = 0.
                    invalid(self, run, 'PolyhedronToFaces', 'nonpositive polyhedron volume',
                            f'partition {partition}', f'cell {cell}')

    def test_stored_volume_must_be_positive_and_match_geometry(self):
        for value in (1. + 1e-6, 0., -1.):
            with self.subTest(volume=value):
                with tempfile.TemporaryDirectory() as tmp:
                    run = Path(tmp)
                    step, _ = cube_fixture(self, run)
                    with h5py.File(step, 'r+') as f:
                        f['VTKHDF/cut/CellData/volume'][0] = value
                    invalid(self, run, 'CellData/volume', 'volume mismatch',
                            'partition 0', 'cell 0')

    def test_stored_volume_cursor_in_later_partition(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp)
            step, _ = cube_fixture(self, run, cells=(2, 1))
            with h5py.File(step, 'r+') as f:
                f['VTKHDF/cut/CellData/volume'][2] = 2.
            invalid(self, run, 'CellData/volume', 'volume mismatch', 'partition 1', 'cell 0')

    def test_tolerated_roundoff_and_optional_volume_preserve_bytes(self):
        for value in (1. + 1e-12, None):
            with self.subTest(volume=value):
                with tempfile.TemporaryDirectory() as tmp:
                    run = Path(tmp)
                    step, baseline = cube_fixture(self, run)
                    with h5py.File(step, 'r+') as f:
                        b = f['VTKHDF/cut']
                        if value is None:
                            del b['CellData/volume']
                        else:
                            b['CellData/volume'][0] = value
                    success(self, run, baseline)

    def test_volume_shape_dtype_and_finiteness(self):
        cases = [np.array([], dtype='float64'), np.array([1., 1.]),
                 np.array([[1.]]), np.array([1.], dtype='float32'),
                 np.array([1], dtype='int64'), np.array([np.nan]),
                 np.array([np.inf]), np.array([-np.inf])]
        for values in cases:
            with self.subTest(shape=values.shape, dtype=values.dtype, values=values):
                with tempfile.TemporaryDirectory() as tmp:
                    run = Path(tmp)
                    step, _ = cube_fixture(self, run)
                    with h5py.File(step, 'r+') as f:
                        replace(f['VTKHDF/cut'], 'CellData/volume', values)
                    invalid(self, run, 'CellData/volume')


if __name__ == '__main__':
    unittest.main(argv=[__file__])
