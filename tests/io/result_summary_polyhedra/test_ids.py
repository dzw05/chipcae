# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
from pathlib import Path
import tempfile
import unittest
from polyhedra_fixtures import cube_fixture, h5py, invalid, replace


class Ids(unittest.TestCase):
    def test_ids_are_local_and_nonnegative_in_each_partition(self):
        for name, stride, limit in [('FaceConnectivity', 24, 8),
                                    ('PolyhedronToFaces', 6, 6)]:
            for partition in (0, 1):
                for bad_id in (-1, limit):
                    with self.subTest(dataset=name, partition=partition, bad_id=bad_id):
                        with tempfile.TemporaryDirectory() as tmp:
                            run = Path(tmp)
                            step, _ = cube_fixture(self, run, cells=(1, 1))
                            with h5py.File(step, 'r+') as f:
                                f['VTKHDF/cut/' + name][partition * stride] = bad_id
                            invalid(self, run, name, f'partition {partition}')

    def test_duplicate_faces_in_one_cell_or_shared_between_cells(self):
        for cells, target in [((1, 0), 1), ((2, 0), 6)]:
            with self.subTest(cells=cells):
                with tempfile.TemporaryDirectory() as tmp:
                    run = Path(tmp)
                    step, _ = cube_fixture(self, run, cells=cells)
                    with h5py.File(step, 'r+') as f:
                        f['VTKHDF/cut/PolyhedronToFaces'][target] = 0
                    invalid(self, run, 'PolyhedronToFaces', 'partition 0')

    def test_omitted_face_with_consistent_reference_allocation(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp)
            step, _ = cube_fixture(self, run)
            with h5py.File(step, 'r+') as f:
                b = f['VTKHDF/cut']
                # Five faces meet the cell minimum; only ownership completeness fails.
                replace(b, 'PolyhedronToFaces', [0, 1, 2, 3, 4], 'int64')
                replace(b, 'NumberOfPolyhedronToFaceIds', [5, 0], 'int64')
                replace(b, 'PolyhedronOffsets', [0, 5, 0], 'int64')
            invalid(self, run, 'PolyhedronToFaces', 'partition 0')


if __name__ == '__main__':
    unittest.main(argv=[__file__])
