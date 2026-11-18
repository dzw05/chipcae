# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
from pathlib import Path
import tempfile
import unittest
from polyhedra_fixtures import TOPOLOGY, cube_fixture, h5py, invalid


class Partial(unittest.TestCase):
    def test_each_missing_array_and_case_sensitive_name(self):
        for name, rename in [(name, False) for name in TOPOLOGY] + [('FaceOffsets', True)]:
            with self.subTest(dataset=name, rename=rename):
                with tempfile.TemporaryDirectory() as tmp:
                    run = Path(tmp)
                    step, _ = cube_fixture(self, run)
                    with h5py.File(step, 'r+') as f:
                        b = f['VTKHDF/cut']
                        if rename:
                            b.move(name, 'faceoffsets')
                        else:
                            del b[name]
                    invalid(self, run, name, 'incomplete polyhedron topology',
                            subprocess_cli=True if rename else False)


if __name__ == '__main__':
    unittest.main(argv=[__file__])
