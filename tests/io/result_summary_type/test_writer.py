# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from type_fixtures import ROOT, h5py, np, success

BUILD = os.environ.get('CHIPCAE_BINARY_DIR')
WRITER = Path(BUILD) / 'io/chipcae_write_results' if BUILD else None
AVAILABLE = WRITER is not None and WRITER.is_file() and os.access(WRITER, os.X_OK)


class Writer(unittest.TestCase):
    def assert_type(self, block, expected):
        self.assertIn('Type', block.attrs)
        self.assertEqual(block.attrs.get_id('Type').shape, ())
        self.assertEqual(block.attrs.get_id('Type').dtype.kind, 'S')
        self.assertEqual(bytes(block.attrs['Type']).rstrip(b'\0').decode('ascii'), expected)

    def test_normal_and_cut_writer_metadata_roundtrip(self):
        if not AVAILABLE:
            self.skipTest('chipcae_write_results unavailable')
        for mode in ((), ('cut',)):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as tmp:
                run = Path(tmp) / 'run'
                completed = subprocess.run(
                    [str(WRITER), str(run), str(ROOT / 'tools/samples/s1.stack.json'), *mode],
                    capture_output=True, timeout=20)
                self.assertEqual(completed.returncode, 0, completed.stderr.decode())
                manifest = json.loads((run / 'manifest.json').read_text(encoding='utf-8'))
                self.assertTrue(manifest['steps'])
                for entry in manifest['steps']:
                    with self.subTest(step=entry['file']), h5py.File(run / entry['file'], 'r') as f:
                        root = f['VTKHDF']
                        self.assert_type(root, 'PartitionedDataSetCollection')
                        for name, text, index in [('hex', 'UnstructuredGrid', 0),
                                                  ('cut', 'UnstructuredGrid', 1),
                                                  ('eb_surface', 'PolyData', 2),
                                                  ('particles', 'PolyData', 3)]:
                            block = root[name]
                            self.assert_type(block, text)
                            self.assertIn('Index', block.attrs)
                            self.assertEqual(block.attrs.get_id('Index').shape, ())
                            value = block.attrs['Index']
                            self.assertIsInstance(value, (int, np.integer))
                            self.assertNotIsInstance(value, (bool, np.bool_))
                            self.assertEqual(int(value), index)
                success(self, run, expected=None)


if __name__ == '__main__':
    if not AVAILABLE:
        print('SKIP: chipcae_write_results unavailable')
        raise SystemExit(77)
    unittest.main(argv=[__file__])
