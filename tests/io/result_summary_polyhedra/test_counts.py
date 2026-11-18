# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
from pathlib import Path
import tempfile
import unittest
from polyhedra_fixtures import ARRAYS, COUNTS, cube_fixture, h5py, invalid, np, replace


class Counts(unittest.TestCase):
    def test_count_dtype_shape_and_sign(self):
        for name in COUNTS:
            for fault in ('int32', 'short', 'long', 'negative', '2d'):
                with self.subTest(dataset=name, fault=fault):
                    with tempfile.TemporaryDirectory() as tmp:
                        run = Path(tmp)
                        step, _ = cube_fixture(self, run)
                        with h5py.File(step, 'r+') as f:
                            b = f['VTKHDF/cut']
                            values = b[name][...]
                            values = {'int32': values.astype('int32'),
                                      'short': values[:1],
                                      'long': np.append(values, np.int64(0)),
                                      'negative': np.array([values[0], -1], dtype='int64'),
                                      '2d': values.reshape(1, 2)}[fault]
                            replace(b, name, values)
                        invalid(self, run, name)

    def test_connectivity_and_offset_dtype(self):
        for name in ARRAYS:
            with self.subTest(dataset=name):
                with tempfile.TemporaryDirectory() as tmp:
                    run = Path(tmp)
                    step, _ = cube_fixture(self, run)
                    with h5py.File(step, 'r+') as f:
                        b = f['VTKHDF/cut']
                        replace(b, name, b[name][...].astype('int32'))
                    invalid(self, run, name)


if __name__ == '__main__':
    unittest.main(argv=[__file__])
