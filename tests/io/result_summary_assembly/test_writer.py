# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from assembly_fixtures import CATEGORIES, LINKS, ROOT, h5py, success

BUILD = os.environ.get('CHIPCAE_BINARY_DIR')
WRITER = Path(BUILD) / 'io/chipcae_write_results' if BUILD else None
AVAILABLE = WRITER is not None and WRITER.is_file() and os.access(WRITER, os.X_OK)


class Writer(unittest.TestCase):
    def test_normal_and_cut_assembly_and_every_step_roundtrip(self):
        for mode in ((), ('cut',)):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as tmp:
                run = Path(tmp) / 'run'
                completed = subprocess.run(
                    [str(WRITER), str(run), str(ROOT / 'tools/samples/s1.stack.json'), *mode],
                    capture_output=True, timeout=20)
                self.assertEqual(completed.returncode, 0, completed.stderr.decode('utf-8'))
                manifest = json.loads((run / 'manifest.json').read_text(encoding='utf-8'))
                self.assertTrue(manifest['steps'])
                for entry in manifest['steps']:
                    with self.subTest(mode=mode, step=entry['n']):
                        with h5py.File(run / entry['file'], 'r') as f:
                            root = f['VTKHDF']
                            assembly = root['Assembly']
                            self.assertIsInstance(assembly, h5py.Group)
                            self.assertEqual(set(assembly), set(CATEGORIES))
                            for category in CATEGORIES:
                                self.assertIsInstance(assembly[category], h5py.Group)
                            for path, target in LINKS:
                                link = assembly.get(path, getlink=True)
                                self.assertIsInstance(link, h5py.SoftLink, path)
                                self.assertEqual(link.path, target)
                                self.assertIsInstance(f.get(link.path), h5py.Group, path)
                        output = success(self, run, expected=None, step=entry['n'])
                        self.assertEqual(json.loads(output)['step_file'], entry['file'])


if __name__ == '__main__':
    if not AVAILABLE:
        print('SKIP: chipcae_write_results unavailable')
        raise SystemExit(77)
    unittest.main(argv=[__file__])
