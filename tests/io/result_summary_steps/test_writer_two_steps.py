# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from steps_fixtures import ROOT, success, assert_summary


class WriterTwoSteps(unittest.TestCase):
    def test_default_writer_manifest(self):
        build = os.environ.get('CHIPCAE_BINARY_DIR')
        writer = Path(build) / 'io/chipcae_write_results' if build else None
        if writer is None or not writer.is_file() or not os.access(writer, os.X_OK):
            self.skipTest('chipcae_write_results unavailable')
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp) / 'run'
            generated = subprocess.run(
                [str(writer), str(run), str(ROOT / 'tools/samples/s1.stack.json')],
                capture_output=True, timeout=20)
            self.assertEqual(generated.returncode, 0, generated.stderr.decode())
            manifest = json.loads((run / 'manifest.json').read_text(encoding='utf-8'))
            self.assertEqual([entry['n'] for entry in manifest['steps']], [0, 1])
            self.assertEqual(len(manifest['summary']), 6)
            contents = {}
            for label, args, selector, n in [
                    ('default', (), None, 0), ('0', ('--step', '0'), 0, 0),
                    ('1', ('--step', '1'), 1, 1),
                    ('last', ('--step', 'last'), 'last', 1)]:
                with self.subTest(selection=label):
                    result, content = success(self, run, args, selector)
                    self.assertEqual(result['step_file'], manifest['steps'][n]['file'])
                    entries = [(entry['name'], entry['value'])
                               for entry in manifest['summary'] if entry['step'] == n]
                    self.assertEqual(len(entries), 3)
                    self.assertEqual({name for name, _ in entries},
                                     {'warpage_pv', 'T_max', 'U_mag_max'})
                    assert_summary(self, result, entries)
                    contents[label] = content
            self.assertEqual(set(contents), {'default', '0', '1', 'last'})
            self.assertEqual(contents['default'], contents['0'])
            self.assertEqual(contents['last'], contents['1'])


if __name__ == '__main__':
    unittest.main(argv=[__file__])
