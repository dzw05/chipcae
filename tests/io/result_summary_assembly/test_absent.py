# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
from pathlib import Path
import tempfile
import unittest
from assembly_fixtures import fixture, success


class Absent(unittest.TestCase):
    def test_unchanged_fixture_preserves_bytes_and_is_repeatable(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp) / 'run'
            fixture(run)
            first = success(self, run)
            self.assertEqual(success(self, run), first)


if __name__ == '__main__':
    unittest.main(argv=[__file__])
