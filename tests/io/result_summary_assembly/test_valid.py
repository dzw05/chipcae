# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
from pathlib import Path
import tempfile
import unittest
from assembly_fixtures import add_assembly, fixture, h5py, success


class Valid(unittest.TestCase):
    def test_complete_reordered_and_extra_category_content_preserve_bytes(self):
        for categories, junk in [
                (('volume', 'boundary', 'lagrangian'), False),
                (('lagrangian', 'volume', 'boundary'), False),
                (('volume', 'boundary', 'lagrangian'), True)]:
            with self.subTest(categories=categories, junk=junk), tempfile.TemporaryDirectory() as tmp:
                run = Path(tmp) / 'run'
                step, _ = fixture(run)
                before = success(self, run)
                with h5py.File(step, 'r+') as f:
                    assembly = add_assembly(f['VTKHDF'], categories)
                    self.assertEqual(list(assembly), list(categories))
                    if junk:
                        assembly.create_group('volume/junk')
                first = success(self, run, expected=before)
                self.assertEqual(success(self, run, expected=before), first)


if __name__ == '__main__':
    unittest.main(argv=[__file__])
