# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
from pathlib import Path
import tempfile
import unittest
from mesh_fixtures import fixture, save, invalid_integer


class Types(unittest.TestCase):
    def test_each_integer_key_rejects_every_noninteger_json_category(self):
        # Keep the contract's key order explicit. Every input has one invalid key.
        for key in ('nx0', 'ny0', 'L', 'padded_cells', 'nz'):
            for value in (True, False, 4.0, '4', None, [], {}):
                with self.subTest(key=key, value=value), tempfile.TemporaryDirectory() as tmp:
                    run = Path(tmp) / 'run'
                    _, manifest = fixture(run)
                    manifest['mesh'][key] = value
                    save(run, manifest)
                    invalid_integer(self, run, key)


if __name__ == '__main__':
    unittest.main(argv=[__file__])
