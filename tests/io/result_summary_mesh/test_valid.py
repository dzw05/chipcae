# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
from pathlib import Path
import tempfile
import unittest
from mesh_fixtures import (
    fixture, save, success, BASELINE_NO_HASH, BASELINE_EMPTY_HASH, VALID_INTEGERS,
)


class Valid(unittest.TestCase):
    def test_complete_mesh_preserves_original_output_and_is_repeatable(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp) / 'run'
            _, manifest = fixture(run)
            original, original_bytes = success(self, run)
            manifest['mesh'].update(VALID_INTEGERS)
            save(run, manifest)
            result, first = success(self, run)
            self.assertEqual(result['mesh_sha256'], 'synthetic-sha256')
            self.assertEqual(list(result), list(original))
            self.assertEqual(first, original_bytes)
            self.assertEqual(success(self, run)[1], first)

    def test_zero_negative_and_beyond_int64_values_are_accepted(self):
        for key, value in (('padded_cells', 0), ('nx0', -1), ('nx0', 2**100)):
            with self.subTest(key=key, value=value), tempfile.TemporaryDirectory() as tmp:
                run = Path(tmp) / 'run'
                _, manifest = fixture(run)
                manifest['mesh'].update(VALID_INTEGERS)
                manifest['mesh'][key] = value
                save(run, manifest)
                result, _ = success(self, run)
                self.assertEqual(result['mesh_sha256'], 'synthetic-sha256')

    def test_partial_mesh_requires_no_other_integer_keys_or_hash(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp) / 'run'
            _, manifest = fixture(run)
            manifest['mesh'] = {'nx0': 4}
            save(run, manifest)
            result, _ = success(self, run, BASELINE_NO_HASH)
            self.assertIsNone(result['mesh_sha256'])

    def test_empty_hash_string_remains_legal(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp) / 'run'
            _, manifest = fixture(run)
            manifest['mesh'].update(VALID_INTEGERS)
            manifest['mesh']['mesh_sha256'] = ''
            save(run, manifest)
            result, _ = success(self, run, BASELINE_EMPTY_HASH)
            self.assertEqual(result['mesh_sha256'], '')


if __name__ == '__main__':
    unittest.main(argv=[__file__])
