# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
import json
import tempfile
import unittest
from pathlib import Path
from steps_fixtures import fixture, cli, api, fixed_keys
from fixtures import baseline_for_numpy


# Captured from the unchanged merged #103 reader at baa0163, before this test
# commit. Keep literal bytes/layout independent of the current reader output.
BASELINE = baseline_for_numpy('''{
  "schema": "chipcae.vv.result_summary/0.1",
  "step_file": "out/step-0.vtkhdf",
  "mesh_sha256": "synthetic-sha256",
  "blocks": {
    "hex": {
      "points": 11,
      "cells": 2,
      "cell_kind_counts": {
        "0": 1,
        "1": 1
      }
    },
    "cut": {
      "points": 0,
      "cells": 0,
      "type_counts": {}
    },
    "eb_surface": {
      "points": 7,
      "polygons": 3
    }
  },
  "layer_names": [
    "substrate",
    "die_attach",
    "Si"
  ],
  "material_names": [
    "铜",
    "silicon"
  ],
  "temperature": {
    "min": 300.0,
    "max": 309.0,
    "top_min": 301.0,
    "top_max": 309.0,
    "top_mean": 305.0
  },
  "warpage": {
    "manifest_warpage_pv_m": 0.1,
    "w_bfp_m": 0.1250000000000002,
    "rel_diff": 0.2500000000000019,
    "n_top_points": 9
  },
  "manifest_summary": {
    "T_max": 309.0,
    "warpage_pv": 0.1
  }
}
''').encode('utf-8')


class Legacy(unittest.TestCase):
    def test_original_fixture_bytes_and_output_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp) / 'run'
            fixture(run)
            contents = []
            for args, selector in [((), None), (('--step', '0'), 0),
                                   (('--step', 'last'), 'last')]:
                with self.subTest(args=args):
                    completed = cli(run, *args)
                    self.assertEqual(completed.returncode, 0, completed.stderr.decode())
                    self.assertEqual(completed.stderr, b'')
                    self.assertEqual(completed.stdout, BASELINE)
                    result = json.loads(completed.stdout)
                    fixed_keys(self, result)
                    expected = api(self, run) if selector is None else api(self, run, selector)
                    self.assertEqual(result, expected)
                    canonical = (json.dumps(expected, indent=2, sort_keys=False,
                                            ensure_ascii=False, allow_nan=False) + '\n').encode('utf-8')
                    self.assertEqual(completed.stdout, canonical)
                    repeat = cli(run, *args)
                    self.assertEqual(repeat.returncode, 0, repeat.stderr.decode())
                    self.assertEqual(repeat.stderr, b'')
                    self.assertEqual(repeat.stdout, completed.stdout)
                    output = Path(tmp) / 'summary.json'
                    written = cli(run, *args, '-o', str(output))
                    self.assertEqual(written.returncode, 0, written.stderr.decode())
                    self.assertEqual(written.stdout, b'')
                    self.assertEqual(written.stderr, b'')
                    self.assertEqual(output.read_bytes(), completed.stdout)
                    contents.append(completed.stdout)
            self.assertEqual(len(contents), 3)
            self.assertEqual(contents, [BASELINE] * 3)


if __name__ == '__main__':
    unittest.main(argv=[__file__])
