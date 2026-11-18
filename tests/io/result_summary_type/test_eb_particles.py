# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
import json
from pathlib import Path
import tempfile
import unittest
from type_fixtures import BASELINE_TEXT, fixture, h5py, invalid, np, replace_attr, serialized, success


class EbParticles(unittest.TestCase):
    def test_legacy_absent_pairs_and_complete_pairs_preserve_bytes(self):
        for name, index in (('eb_surface', 2), ('particles', 3)):
            for complete in (False, True):
                with self.subTest(block=name, complete=complete), \
                        tempfile.TemporaryDirectory() as tmp:
                    run = Path(tmp) / 'run'
                    step, _ = fixture(run)
                    with h5py.File(step, 'r+') as f:
                        block = f['VTKHDF'].require_group(name)
                        if complete:
                            replace_attr(block, 'Type', np.bytes_('PolyData'))
                            replace_attr(block, 'Index', index)
                        if name == 'particles':
                            self.assertNotIn('Version', block.attrs)
                            self.assertEqual(list(block), [])
                    result, _ = success(self, run)
                    self.assertNotIn('particles', result['blocks'])

    def test_partial_pairs_are_invalid(self):
        for name, index in (('eb_surface', 2), ('particles', 3)):
            for attr, value in (('Type', np.bytes_('PolyData')), ('Index', index)):
                with self.subTest(block=name, only=attr), tempfile.TemporaryDirectory() as tmp:
                    run = Path(tmp) / 'run'
                    step, _ = fixture(run)
                    with h5py.File(step, 'r+') as f:
                        block = f['VTKHDF'].require_group(name)
                        replace_attr(block, attr, value)
                    invalid(self, run, 'VTKHDF/' + name, 'incomplete Type/Index')

    def test_complete_pairs_require_polydata_and_the_assigned_index(self):
        for name, index in (('eb_surface', 2), ('particles', 3)):
            for attr, value, expected in [('Type', np.bytes_('UnstructuredGrid'), 'PolyData'),
                                          ('Index', index + 1, str(index))]:
                with self.subTest(block=name, attr=attr), tempfile.TemporaryDirectory() as tmp:
                    run = Path(tmp) / 'run'
                    step, _ = fixture(run)
                    with h5py.File(step, 'r+') as f:
                        block = f['VTKHDF'].require_group(name)
                        replace_attr(block, 'Type', np.bytes_('PolyData'))
                        replace_attr(block, 'Index', index)
                        replace_attr(block, attr, value)
                    invalid(self, run, 'VTKHDF/' + name, attr, expected)

    def test_absent_eb_surface_preserves_zero_defaults(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp) / 'run'
            step, _ = fixture(run)
            with h5py.File(step, 'r+') as f:
                del f['VTKHDF/eb_surface']
            expected = json.loads(BASELINE_TEXT)
            expected['blocks']['eb_surface'] = {'points': 0, 'polygons': 0}
            result, _ = success(self, run, serialized(expected))
            self.assertEqual(result['blocks']['eb_surface'], {'points': 0, 'polygons': 0})


if __name__ == '__main__':
    unittest.main(argv=[__file__])
