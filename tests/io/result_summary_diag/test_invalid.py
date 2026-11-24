# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
from pathlib import Path
import tempfile
import unittest
from diag_fixtures import baseline, fixture, h5py, insert, invalid, np


class Invalid(unittest.TestCase):
    def test_malformed_diagnostics_are_rejected_repeatably(self):
        cases = (
            ('hex', 'copper_fraction', True, np.zeros(5, dtype='float64'),
             'expected shape'),
            ('hex', 'conductivity_diag', True, np.zeros(2, dtype='float64'),
             'expected shape'),
            ('cut', 'conductivity_diag', True, np.zeros((2, 2), dtype='float64'),
             'expected shape'),
            ('eb_surface', 'copper_fraction', True, np.zeros(2, dtype='float64'),
             'expected shape'),
            ('cut', 'conductivity_diag', False, np.empty(0, dtype='float64'),
             'expected shape'),
            ('hex', 'copper_fraction', True, np.zeros(2, dtype='int32'),
             'expected float64'),
            ('hex', 'conductivity_diag', True, np.zeros((2, 3), dtype='float32'),
             'expected float64'),
            ('hex', 'copper_fraction', True, np.array([0.5, np.nan]),
             'nonfinite values'),
            ('cut', 'conductivity_diag', True,
             np.array([[1., 2., 3.], [4., np.inf, 6.]]), 'nonfinite values'),
            ('eb_surface', 'conductivity_diag', True,
             np.array([[1., 2., 3.], [4., 5., 6.], [7., 8., -np.inf]]),
             'nonfinite values'),
            ('hex', 'copper_fraction', True, None, 'expected shape'),
        )
        for block, field, cut, values, phrase in cases:
            with self.subTest(block=block, field=field, cut=cut, phrase=phrase,
                              group=values is None), tempfile.TemporaryDirectory() as tmp:
                run = Path(tmp) / 'run'
                step, _ = fixture(run, cut=cut)
                baseline(self, run, step)
                if values is None:
                    with h5py.File(step, 'a') as handle:
                        handle.create_group(f'VTKHDF/{block}/CellData/{field}')
                else:
                    insert(step, block, **{field: values})
                invalid(self, run, block, field, phrase)


if __name__ == '__main__':
    unittest.main(argv=[__file__])
