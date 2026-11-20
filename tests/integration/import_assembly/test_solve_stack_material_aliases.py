"""Regressions for solve_stack's material fixture, without running the solver."""
import copy
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import assembly_test_support as support


for backend in ('MFEM', 'MPI', 'HDF5'):
    support.require(backend)


class MaterialAliases(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / 'source'
        self.examples = self.source / 'schema/examples'
        self.examples.mkdir(parents=True)
        self.original = json.loads(
            (support.SOURCE / 'schema/examples/fcbga_demo.materials.json').read_text())
        case = support.SOURCE / 'schema/examples/fcbga_demo.case.json'
        (self.examples / case.name).write_text(case.read_text())
        self.addCleanup(patch.stopall)
        patch.object(support, 'SOURCE', self.source).start()
        # Only intercept process launch: solve_stack still reads, normalizes and
        # writes its real material/case/profile files, then reads the manifest.
        patch.object(support.subprocess, 'run', side_effect=self.solver_result).start()

    @staticmethod
    def solver_result(command, **kwargs):
        destination = Path(command[command.index('--out') + 1])
        destination.mkdir()
        (destination / 'manifest.json').write_text('{"fixture_manifest": true}')
        return subprocess.CompletedProcess(command, 0, stdout='', stderr='')

    def solve(self, library, name='first'):
        (self.examples / 'fcbga_demo.materials.json').write_text(json.dumps(library))
        output = self.root / name
        output.mkdir()
        manifest = support.solve_stack(output, output / 'stack.json', 'result')
        self.assertEqual(manifest, {'fixture_manifest': True})
        return json.loads((output / 'materials.json').read_text())

    def test_current_library_has_unique_ids_in_source_order(self):
        result = self.solve(self.original)
        ids = [card['id'] for card in result['materials']]
        self.assertEqual(ids, ['organic_eq', 'sac305_ball_eq', 'underfill',
                               'silicon', 'copper', 'fr4_eq'])
        self.assertEqual(ids.count('copper'), 1)
        self.assertEqual(len(ids), len(set(ids)))

    def test_existing_copper_is_the_normalized_source_card(self):
        expected = copy.deepcopy(self.original['materials'][-1])
        for section in ('elastic', 'thermal_expansion'):
            table = expected[section]['table']
            table['temperatures'] = [250]
            table['columns'] = {key: [values[0]] for key, values in table['columns'].items()}
            table['interpolation'] = 'constant'
            table['extrapolation'] = 'clamp'
        cards = self.solve(self.original)['materials']
        self.assertEqual([card for card in cards if card['id'] == 'copper'], [expected])

    def test_only_missing_aliases_are_appended_with_exact_case(self):
        # Distinct density values expose replacing an existing alias with a
        # clone of the first card. Source order deliberately differs from alias order.
        scenarios = [
            (['base'], ['base', 'fr4_eq', 'copper']),
            (['base', 'copper', 'fr4_eq'], ['base', 'copper', 'fr4_eq']),
            (['base', 'fr4_eq'], ['base', 'fr4_eq', 'copper']),
            (['base', 'copper'], ['base', 'copper', 'fr4_eq']),
            (['base', 'Copper'], ['base', 'Copper', 'fr4_eq', 'copper']),
        ]
        for index, (source_ids, expected_ids) in enumerate(scenarios):
            with self.subTest(source_ids=source_ids):
                library = copy.deepcopy(self.original)
                library['materials'] = []
                for offset, id_ in enumerate(source_ids):
                    card = copy.deepcopy(self.original['materials'][0])
                    card.update(id=id_, density=1000 + offset)
                    library['materials'].append(card)
                cards = self.solve(library, f'scenario-{index}')['materials']
                self.assertEqual([card['id'] for card in cards], expected_ids)
                self.assertEqual([card['density'] for card in cards[:len(source_ids)]],
                                 list(range(1000, 1000 + len(source_ids))))
                for card in cards[len(source_ids):]:
                    self.assertEqual(card['density'], 1000)

    def test_separate_calls_reload_the_source_without_shared_state(self):
        first = self.solve(self.original, 'first')
        second = self.solve(self.original, 'second')
        self.assertEqual(first, second)
        self.assertEqual(json.loads(
            (self.examples / 'fcbga_demo.materials.json').read_text()), self.original)
        changed = copy.deepcopy(self.original)
        changed['materials'][0]['density'] = 1234
        reloaded = self.solve(changed, 'changed-source')
        self.assertEqual(reloaded['materials'][0]['density'], 1234)


if __name__ == '__main__':
    unittest.main(argv=[__file__], verbosity=2)
