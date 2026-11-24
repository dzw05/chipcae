# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
from pathlib import Path
import tempfile
import unittest
from diag_fixtures import baseline, diagnostics, fixture, insert, np, success


class Valid(unittest.TestCase):
    def test_valid_fields_keep_output_bytes(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp) / 'run'
            step, _ = fixture(run, cut=True)
            original = baseline(self, run, step)
            for block, cells in (('hex', 2), ('cut', 2), ('eb_surface', 3)):
                insert(step, block, **diagnostics(cells))
            success(self, run, original)

    def test_empty_cut_template_accepted(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp) / 'run'
            step, _ = fixture(run, cut=False)
            original = baseline(self, run, step)
            insert(step, 'hex', **diagnostics(2))
            insert(step, 'cut', **diagnostics(0))
            success(self, run, original)

    def test_big_endian_accepted(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp) / 'run'
            step, _ = fixture(run)
            original = baseline(self, run, step)
            insert(step, 'hex', **diagnostics(2, dtype='>f8'))
            success(self, run, original)

    def test_partial_presence_accepted(self):
        cases = [((block, field, cells),)
                 for block, cells in (('hex', 2), ('cut', 2), ('eb_surface', 3))
                 for field in ('copper_fraction', 'conductivity_diag')]
        cases.append((('hex', 'copper_fraction', 2),
                      ('cut', 'conductivity_diag', 2),
                      ('eb_surface', 'copper_fraction', 3)))
        for fields in cases:
            with self.subTest(fields=fields), tempfile.TemporaryDirectory() as tmp:
                run = Path(tmp) / 'run'
                step, _ = fixture(run, cut=True)
                original = baseline(self, run, step)
                for block, field, cells in fields:
                    insert(step, block, **{field: diagnostics(cells)[field]})
                success(self, run, original)

    def test_malformed_differently_cased_aliases_are_ignored(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp) / 'run'
            step, _ = fixture(run, cut=True)
            original = baseline(self, run, step)
            for block in ('hex', 'cut', 'eb_surface'):
                insert(step, block, Copper_Fraction=np.zeros(5, dtype='int32'),
                       Conductivity_Diag=np.array([np.nan], dtype='float32'))
            success(self, run, original)


if __name__ == '__main__':
    unittest.main(argv=[__file__])
