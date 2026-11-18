# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
from pathlib import Path
import tempfile
import unittest
from polyhedra_fixtures import cube_fixture, success


class ValidCube(unittest.TestCase):
    def test_outward_cubes_preserve_summary_bytes(self):
        for side, translation, cells in [(1., 0., (1, 0)), (1., 1e6, (1, 0)),
                                         (2., 0., (1, 0)), (1., 0., (2, 1))]:
            with self.subTest(side=side, translation=translation, cells=cells):
                with tempfile.TemporaryDirectory() as tmp:
                    run = Path(tmp)
                    _, baseline = cube_fixture(self, run, cells, side, translation)
                    result, _ = success(self, run, baseline, subprocess_cli=True)
                    self.assertEqual(result['blocks']['cut']['type_counts'],
                                     {'42': sum(cells)})

    def test_missing_topology_and_correctly_empty_partitions(self):
        for topology, cells in [(False, (1, 0)), (True, (0, 0))]:
            with self.subTest(topology=topology, cells=cells):
                with tempfile.TemporaryDirectory() as tmp:
                    run = Path(tmp)
                    _, baseline = cube_fixture(self, run, cells=cells, topology=topology)
                    result, _ = success(self, run, baseline)
                    self.assertEqual(result['blocks']['cut']['type_counts'],
                                     {'42': 1} if sum(cells) else {})


if __name__ == '__main__':
    unittest.main(argv=[__file__])
