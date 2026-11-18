# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from polyhedra_fixtures import ROOT, TOPOLOGY, h5py, success

BUILD = os.environ.get('CHIPCAE_BINARY_DIR')
WRITER = Path(BUILD) / 'io/chipcae_write_results' if BUILD else None
AVAILABLE = WRITER is not None and WRITER.is_file() and os.access(WRITER, os.X_OK)


class WriterCut(unittest.TestCase):
    def test_existing_writer_cut_roundtrip(self):
        if not AVAILABLE:
            self.skipTest('chipcae_write_results unavailable')
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp) / 'run'
            generated = subprocess.run(
                [str(WRITER), str(run), str(ROOT / 'tools/samples/s1.stack.json'), 'cut'],
                capture_output=True, timeout=20)
            self.assertEqual(generated.returncode, 0, generated.stderr.decode())
            result, original = success(self, run, subprocess_cli=True)
            self.assertEqual(result['blocks']['cut']['type_counts'], {'42': 1})
            # Preserve this writer input's summary bytes without optional face data, too.
            with h5py.File(run / result['step_file'], 'r+') as f:
                b = f['VTKHDF/cut']
                for name in TOPOLOGY + ('CellData/volume',):
                    del b[name]
            success(self, run, original, subprocess_cli=True)


if __name__ == '__main__':
    if not AVAILABLE:
        print('SKIP: chipcae_write_results unavailable')
        raise SystemExit(77)
    unittest.main(argv=[__file__])
