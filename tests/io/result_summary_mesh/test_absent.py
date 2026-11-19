# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
from pathlib import Path
import tempfile
import unittest
from mesh_fixtures import (
    fixture, save, success, BASELINE_NO_HASH, VALID_INTEGERS,
)


class Absent(unittest.TestCase):
    def test_original_fixture_preserves_bytes_and_is_repeatable(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp) / 'run'
            fixture(run)
            result, first = success(self, run)
            self.assertEqual(result['mesh_sha256'], 'synthetic-sha256')
            self.assertEqual(success(self, run)[1], first)

    def test_missing_empty_and_unknown_mesh_members_are_ignored(self):
        for mesh in (None, {}, {'Nx0': 4},
                     {'NX0': False, 'unrelated': {'nx0': None}}):
            with self.subTest(mesh=mesh), tempfile.TemporaryDirectory() as tmp:
                run = Path(tmp) / 'run'
                _, manifest = fixture(run)
                if mesh is None:
                    del manifest['mesh']
                else:
                    manifest['mesh'] = mesh
                save(run, manifest)
                result, _ = success(self, run, BASELINE_NO_HASH)
                self.assertIsNone(result['mesh_sha256'])

    def test_top_level_integer_names_are_ignored(self):
        for nested in (False, True):
            with self.subTest(nested=nested), tempfile.TemporaryDirectory() as tmp:
                run = Path(tmp) / 'run'
                _, manifest = fixture(run)
                manifest.update(nx0=True, ny0=4.0, L='4', padded_cells=None, nz=[])
                if nested:
                    manifest['mesh'].update(VALID_INTEGERS)
                    save(run, manifest)
                    result, _ = success(self, run)
                    self.assertEqual(result['mesh_sha256'], 'synthetic-sha256')
                else:
                    del manifest['mesh']
                    save(run, manifest)
                    result, _ = success(self, run, BASELINE_NO_HASH)
                    self.assertIsNone(result['mesh_sha256'])


if __name__ == '__main__':
    unittest.main(argv=[__file__])
