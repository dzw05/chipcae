# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
from pathlib import Path
import tempfile
import unittest
from type_fixtures import fixture, h5py, invalid, np, replace_attr


class HexCut(unittest.TestCase):
    def test_type_and_index_are_required_with_exact_attribute_spelling(self):
        for name, index in (('hex', 0), ('cut', 1)):
            for attr, expected in (('Type', 'UnstructuredGrid'), ('Index', str(index))):
                for rename in (False, True):
                    with self.subTest(block=name, attr=attr, rename=rename), \
                            tempfile.TemporaryDirectory() as tmp:
                        run = Path(tmp) / 'run'
                        step, _ = fixture(run)
                        with h5py.File(step, 'r+') as f:
                            block = f['VTKHDF/' + name]
                            original = block.attrs[attr]
                            del block.attrs[attr]
                            if rename:
                                block.attrs.create(attr.lower(), original)
                        invalid(self, run, 'VTKHDF/' + name, attr, expected)

    def test_type_must_identify_an_unstructured_grid(self):
        for name in ('hex', 'cut'):
            for value in ('PolyData', 'unstructuredgrid'):
                with self.subTest(block=name, value=value), tempfile.TemporaryDirectory() as tmp:
                    run = Path(tmp) / 'run'
                    step, _ = fixture(run)
                    with h5py.File(step, 'r+') as f:
                        replace_attr(f['VTKHDF/' + name], 'Type', np.bytes_(value))
                    invalid(self, run, 'VTKHDF/' + name, 'Type', 'UnstructuredGrid')

    def test_index_requires_the_exact_scalar_integer(self):
        for name, index in (('hex', 0), ('cut', 1)):
            cases = [('swapped', 1 - index), ('string', str(index)),
                     ('float', float(index)), ('array', np.array([index])),
                     ('bool', bool(index)), ('numpy bool', np.bool_(index))]
            for label, value in cases:
                with self.subTest(block=name, case=label), tempfile.TemporaryDirectory() as tmp:
                    run = Path(tmp) / 'run'
                    step, _ = fixture(run)
                    with h5py.File(step, 'r+') as f:
                        replace_attr(f['VTKHDF/' + name], 'Index', value)
                    invalid(self, run, 'VTKHDF/' + name, 'Index', str(index))


if __name__ == '__main__':
    unittest.main(argv=[__file__])
