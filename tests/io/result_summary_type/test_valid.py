# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
from pathlib import Path
import tempfile
import unittest
from type_fixtures import fixture, h5py, np, replace_attr, success


class Valid(unittest.TestCase):
    def test_unchanged_fixture_preserves_bytes_and_is_repeatable(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp) / 'run'
            fixture(run)
            _, first = success(self, run)
            _, second = success(self, run)
            self.assertEqual(first, second)

    def test_fixed_byte_types_allow_trailing_nuls(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp) / 'run'
            step, _ = fixture(run)
            with h5py.File(step, 'r+') as f:
                root = f['VTKHDF']
                root.create_group('particles')
                for name, text, index in [
                        ('', 'PartitionedDataSetCollection', None),
                        ('hex', 'UnstructuredGrid', 0),
                        ('cut', 'UnstructuredGrid', 1),
                        ('eb_surface', 'PolyData', 2),
                        ('particles', 'PolyData', 3)]:
                    block = root[name] if name else root
                    value = text.encode('ascii') + b'\0\0\0'
                    replace_attr(block, 'Type', value, dtype=f'S{len(value)}')
                    if index is not None:
                        replace_attr(block, 'Index', index)
            success(self, run)

    def test_integer_widths_and_byte_orders_are_accepted(self):
        for dtype in ('i1', 'u1', '<i2', '>u2', '<i4', '>u4', '<i8', '>u8'):
            with self.subTest(dtype=dtype), tempfile.TemporaryDirectory() as tmp:
                run = Path(tmp) / 'run'
                step, _ = fixture(run)
                with h5py.File(step, 'r+') as f:
                    root = f['VTKHDF']
                    root.create_group('particles')
                    for index, name in enumerate(('hex', 'cut', 'eb_surface', 'particles')):
                        block = root[name]
                        if index >= 2:
                            replace_attr(block, 'Type', np.bytes_('PolyData'))
                        replace_attr(block, 'Index', index, dtype=np.dtype(dtype))
                success(self, run)

    def test_root_index_is_ignored_even_when_malformed(self):
        for value in (np.array([9, 8]), 'ignored', True):
            with self.subTest(value=repr(value)), tempfile.TemporaryDirectory() as tmp:
                run = Path(tmp) / 'run'
                step, _ = fixture(run)
                with h5py.File(step, 'r+') as f:
                    replace_attr(f['VTKHDF'], 'Index', value)
                success(self, run)


if __name__ == '__main__':
    unittest.main(argv=[__file__])
