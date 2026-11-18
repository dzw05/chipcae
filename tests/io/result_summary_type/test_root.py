# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
from pathlib import Path
import tempfile
import unittest
from type_fixtures import fixture, h5py, invalid, np, replace_attr


class Root(unittest.TestCase):
    def test_type_is_required_with_exact_attribute_spelling(self):
        for rename in (False, True):
            with self.subTest(rename=rename), tempfile.TemporaryDirectory() as tmp:
                run = Path(tmp) / 'run'
                step, _ = fixture(run)
                with h5py.File(step, 'r+') as f:
                    root = f['VTKHDF']
                    original = root.attrs['Type']
                    del root.attrs['Type']
                    if rename:
                        root.attrs.create('type', original)
                invalid(self, run, 'VTKHDF', 'Type', 'PartitionedDataSetCollection')

    def test_type_requires_exact_ascii_scalar_fixed_bytes(self):
        cases = [
            ('lowercase', np.bytes_('partitioneddatasetcollection'), None),
            ('trailing space', np.bytes_('PartitionedDataSetCollection '), None),
            ('variable UTF-8', 'PartitionedDataSetCollection', h5py.string_dtype()),
            ('integer', 27, None),
            ('array', np.array([b'PartitionedDataSetCollection']), None),
            ('non-ASCII', np.bytes_(b'PartitionedDataSetCollectio\xff'), None),
        ]
        for label, value, dtype in cases:
            with self.subTest(case=label), tempfile.TemporaryDirectory() as tmp:
                run = Path(tmp) / 'run'
                step, _ = fixture(run)
                with h5py.File(step, 'r+') as f:
                    replace_attr(f['VTKHDF'], 'Type', value, dtype)
                invalid(self, run, 'VTKHDF', 'Type', 'PartitionedDataSetCollection')


if __name__ == '__main__':
    unittest.main(argv=[__file__])
