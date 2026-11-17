# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from fixtures import fixture, SCRIPT
from tools.vv.result_summary import summarize


class CLI(unittest.TestCase):
    def test_bytes_and_repeatability(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp)/'run'
            fixture(run)
            command = [sys.executable, str(SCRIPT), str(run)]
            def invoke(args):
                result = subprocess.run(command+args, cwd=tmp, capture_output=True, timeout=20)
                self.assertEqual(result.returncode, 0, result.stderr.decode())
                self.assertEqual(result.stderr, b'')
                return result.stdout
            first = invoke([])
            self.assertEqual(first, invoke([]))
            output = Path(tmp)/'summary.json'
            self.assertEqual(invoke(['-o', str(output)]), b'')
            self.assertEqual(first, output.read_bytes())
            expected = json.dumps(summarize(run), indent=2, sort_keys=False, ensure_ascii=False)+'\n'
            self.assertEqual(first, expected.encode('utf-8'))
            self.assertTrue(first.endswith(b'\n'))
            self.assertEqual(json.loads(invoke(['--top-rel-tol', '0'])), json.loads(first))


if __name__ == '__main__':
    unittest.main(argv=[__file__])
