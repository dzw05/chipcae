"""Synthetic CAD covers provenance and local real-case result recording."""
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from parts_support import DependencyMissing, box, dump, occt, prepare, write_step
import test_e2e_derived_chamfer_lid as derived
import test_e2e_real_bulk_parts as real


class FixtureRecording(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix='sp7-record-regression-')
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.source = self.root / 'source'
        self.source.mkdir()
        prepare(self.source)
        (self.source / 'board.xml').rename(self.source / 'package_substrate.ipc2581.xml')
        # Original synthetic geometry, never reads private vendor inputs.
        write_step(self.source / 'PACKAGE_BULK_PARTS.step', box(0, 0, 0, 56, 44, .5))
        self.directory = self.root / 'run'
        self.directory.mkdir()
        env = patch.dict(os.environ, {'CHIPCAE_DEMO_STEP_DIR': str(self.source)})
        env.start()
        self.addCleanup(env.stop)

    def test_derived_provenance_exists_before_importer_is_available(self):
        # SP4 is absent; stop only at that external boundary, after real generation.
        with patch.object(derived, 'imported', side_effect=DependencyMissing('SP4')):
            with self.assertRaises(DependencyMissing):
                derived.acceptance(self.directory)
        notes = json.loads((self.directory / 'fixture-notes.json').read_text())
        self.assertIn('派生件（由真实盖板四角倒角，非原始设计）', notes['labels'])
        self.assertTrue((self.directory / 'lid_chamfer_derived.step').is_file())

    def test_bulk_writes_annotated_local_result(self):
        # Stand in for unavailable assembly/solver backends; keep real fixture IO.
        def imported(d, model):
            bodies = [*model['parts'], *model['parts_from_step']['bodies'].values()]
            dump(d / 'report.json', {'labels': ['近似材料'],
                                    'bodies': [p['id'] for p in bodies]})
            return {'layers': [{'regions': [{'body': p['id'], 'material': p['material']}
                                            for p in bodies]}]}

        with patch.object(real, 'imported', side_effect=imported), \
                patch.object(real, 'solve', return_value=(1.25e-6, self.directory / 'synthetic.vtkhdf')):
            real.acceptance(self.directory)
        result = json.loads((self.directory / 'result.json').read_text())
        self.assertEqual(result['data_class'], 'real')
        self.assertAlmostEqual(result['warpage_pv'], 1.25e-6, delta=1e-18)
        self.assertIn('近似材料', result['labels'])
        self.assertIn('interposer_approx: 近似材料、近似几何', result['labels'])
        self.assertEqual(result['copper_layers'], '整层平均')


if __name__ == '__main__':
    occt()
    unittest.main(argv=[__file__])
