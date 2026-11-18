# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
from pathlib import Path
import tempfile
import unittest
from polyhedra_fixtures import ARRAYS, cube_fixture, h5py, invalid, replace


class Offsets(unittest.TestCase):
    def test_starts_terminals_and_monotonicity(self):
        cases = [
            ('FaceOffsets', (1, 0), [1, 4, 8, 12, 16, 20, 24, 0]),
            ('FaceOffsets', (1, 0), [0, 4, 8, 12, 16, 20, 23, 0]),
            ('FaceOffsets', (1, 0), [0, 4, 3, 12, 16, 20, 24, 0]),
            ('PolyhedronOffsets', (1, 0), [1, 6, 0]),
            ('PolyhedronOffsets', (1, 0), [0, 5, 0]),
            ('PolyhedronOffsets', (2, 0), [0, 13, 12, 0]),
            # Empty trailing partitions still have an independently checked zero offset.
            ('FaceOffsets', (1, 0), [0, 4, 8, 12, 16, 20, 24, 1]),
            ('PolyhedronOffsets', (1, 0), [0, 6, 1]),
        ]
        for name, cells, values in cases:
            with self.subTest(dataset=name, values=values):
                with tempfile.TemporaryDirectory() as tmp:
                    run = Path(tmp)
                    step, _ = cube_fixture(self, run, cells=cells)
                    with h5py.File(step, 'r+') as f:
                        replace(f['VTKHDF/cut'], name, values, 'int64')
                    invalid(self, run, name, 'partition')

    def test_exact_aggregate_lengths(self):
        for name in ARRAYS:
            for delta in (-1, 1):
                with self.subTest(dataset=name, delta=delta):
                    with tempfile.TemporaryDirectory() as tmp:
                        run = Path(tmp)
                        step, _ = cube_fixture(self, run)
                        with h5py.File(step, 'r+') as f:
                            b = f['VTKHDF/cut']
                            values = b[name][...].tolist()
                            replace(b, name, values[:-1] if delta < 0 else values + [0],
                                    'int64')
                        invalid(self, run, name)

    def test_face_needs_three_points(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp)
            step, _ = cube_fixture(self, run)
            with h5py.File(step, 'r+') as f:
                b = f['VTKHDF/cut']
                # Lengths and endpoints agree; only the first ring is too short.
                replace(b, 'FaceConnectivity', b['FaceConnectivity'][...][2:])
                replace(b, 'NumberOfFaceConnectivityIds', [22, 0], 'int64')
                replace(b, 'FaceOffsets', [0, 2, 6, 10, 14, 18, 22, 0], 'int64')
            invalid(self, run, 'FaceOffsets', 'partition 0')

    def test_cell_needs_four_faces(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp)
            step, _ = cube_fixture(self, run, cells=(2, 0))
            with h5py.File(step, 'r+') as f:
                # All twelve faces are still uniquely owned, but cell zero has only three.
                replace(f['VTKHDF/cut'], 'PolyhedronOffsets', [0, 3, 12, 0], 'int64')
            invalid(self, run, 'PolyhedronOffsets', 'partition 0')

    def test_nonempty_cell_with_no_faces(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp)
            step, _ = cube_fixture(self, run)
            with h5py.File(step, 'r+') as f:
                b = f['VTKHDF/cut']
                for name in ('NumberOfFaces', 'NumberOfFaceConnectivityIds',
                             'NumberOfPolyhedronToFaceIds'):
                    replace(b, name, [0, 0], 'int64')
                for name, values in [('FaceOffsets', [0, 0]),
                                     ('PolyhedronOffsets', [0, 0, 0]),
                                     ('FaceConnectivity', []), ('PolyhedronToFaces', [])]:
                    replace(b, name, values, 'int64')
            invalid(self, run, 'PolyhedronOffsets', 'partition 0')


if __name__ == '__main__':
    unittest.main(argv=[__file__])
