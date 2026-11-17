# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
import tempfile
import unittest
from pathlib import Path
from fixtures import fixture, fixed_keys
from tools.vv.result_summary import summarize


class WithCut(unittest.TestCase):
    def test_cut_contributes(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp)
            fixture(run, cut=True)
            r = summarize(run)
            fixed_keys(self, r)
            self.assertEqual(r['blocks']['cut'], {'points': 2, 'cells': 2, 'type_counts': {'42': 2}})
            self.assertEqual(r['temperature']['min'], 100.)
            self.assertEqual(r['temperature']['max'], 999.)
            self.assertEqual(r['temperature']['top_min'], 301.)
            self.assertEqual(r['temperature']['top_max'], 999.)
            self.assertAlmostEqual(r['temperature']['top_mean'], (305.*9+999)/10, delta=374.4e-14)
            self.assertEqual(r['warpage']['n_top_points'], 10)


if __name__ == '__main__':
    unittest.main(argv=[__file__])
