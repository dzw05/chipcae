"""Volume readback accepts empty cuts without hiding missing nonempty volumes."""
import tempfile
import unittest
from pathlib import Path

try:
    import h5py
    import numpy as np
except ImportError:
    print('SKIP: volume readback requires h5py and numpy')
    raise SystemExit(77)

from parts_support import volume


class VolumeReadback(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix='sp7-volume-')
        self.addCleanup(directory.cleanup)
        self.path = Path(directory.name) / 'result.vtkhdf'
        with h5py.File(self.path, 'w') as f:
            for block in ('hex', 'cut'):
                g = f.create_group('VTKHDF/' + block)
                g['FieldData/material_names'] = np.array([b'silicon', b'underfill'])
                g['CellData/material_id'] = np.array([1] if block == 'hex' else [], dtype='i4')
            g = f['VTKHDF/hex']
            g['NumberOfPoints'] = [8]
            g['NumberOfCells'] = [1]
            g['Connectivity'] = np.arange(8)
            # A 2 x 3 x 4 mm hex has physical volume 24e-9 m^3.
            g['Points'] = [[0, 0, 0], [.002, 0, 0], [.002, .003, 0], [0, .003, 0],
                           [0, 0, .004], [.002, 0, .004], [.002, .003, .004], [0, .003, .004]]

    def test_empty_cut_without_volume_preserves_hex_volume(self):
        self.assertAlmostEqual(volume(self.path, 'silicon'), 24e-9, delta=1e-18)

    def test_empty_cut_with_volume_preserves_hex_volume(self):
        with h5py.File(self.path, 'a') as f:
            f['VTKHDF/cut/CellData/volume'] = np.array([], dtype='f8')
        self.assertAlmostEqual(volume(self.path, 'silicon'), 24e-9, delta=1e-18)

    def test_nonempty_cut_adds_only_selected_material_volume(self):
        with h5py.File(self.path, 'a') as f:
            g = f['VTKHDF/cut/CellData']
            del g['material_id']
            g['material_id'] = [1, 2]
            g['volume'] = [5e-9, 7e-9]
        self.assertAlmostEqual(volume(self.path, 'silicon'), 29e-9, delta=1e-18)

    def test_nonempty_cut_without_volume_is_rejected(self):
        with h5py.File(self.path, 'a') as f:
            g = f['VTKHDF/cut/CellData']
            del g['material_id']
            g['material_id'] = [1]
        with self.assertRaises(KeyError):
            volume(self.path, 'silicon')


if __name__ == '__main__':
    unittest.main(argv=[__file__])
