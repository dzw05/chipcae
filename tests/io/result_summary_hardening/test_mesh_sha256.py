# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
from pathlib import Path
import tempfile
import unittest
from hardening_fixtures import fixture, save, cli, invalid_cli, invalid_api, success


class MeshSHA256(unittest.TestCase):
    def test_present_hash_must_be_a_string(self):
        for value in (123, {}, None, True):
            with self.subTest(value=value), tempfile.TemporaryDirectory() as tmp:
                run = Path(tmp) / 'run'
                _, manifest = fixture(run)
                manifest['mesh']['mesh_sha256'] = value
                save(run, manifest)
                with self.subTest(interface='CLI'):
                    invalid_cli(self, cli(run), 'mesh.mesh_sha256')
                with self.subTest(interface='API'):
                    invalid_api(self, run, 'mesh.mesh_sha256')

    def test_empty_string_is_preserved(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp) / 'run'
            _, manifest = fixture(run)
            manifest['mesh']['mesh_sha256'] = ''
            save(run, manifest)
            self.assertEqual(success(self, run)['mesh_sha256'], '')

    def test_only_exact_key_is_used_and_absence_is_null(self):
        # A numeric wrong-case key must be ignored, including its value's type.
        for mesh in ({'Mesh_SHA256': 123}, {}, None):
            with self.subTest(mesh=mesh), tempfile.TemporaryDirectory() as tmp:
                run = Path(tmp) / 'run'
                _, manifest = fixture(run)
                if mesh is None:
                    del manifest['mesh']
                else:
                    manifest['mesh'] = mesh
                save(run, manifest)
                self.assertIsNone(success(self, run)['mesh_sha256'])


if __name__ == '__main__':
    unittest.main(argv=[__file__])
