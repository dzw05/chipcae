"""Optional import dependencies must not hide unrelated acceptance failures."""
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

from parts_support import DependencyMissing, main, prepare, run


class ImportDependencies(unittest.TestCase):
    def invoke(self, name='import', code=7,
               message='IMPORT_REJECTED: ECAD support disabled', expected=0):
        # Substitute only CLI discovery; exercise real subprocess/error handling.
        with patch('parts_support.cli', return_value=sys.executable):
            return run(name, ['-c', 'import sys; sys.stderr.write(sys.argv[1]); '
                              'sys.exit(int(sys.argv[2]))', message, code], expected=expected)

    def test_exact_ecad_disabled_import_is_a_dependency(self):
        with self.assertRaisesRegex(DependencyMissing, 'import built without ECAD:'):
            self.invoke(message='IMPORT_REJECTED: ECAD support disabled\n')

    def test_other_rejections_remain_assertion_failures(self):
        cases = [('import', 7, 'IMPORT_REJECTED: C-tier defects'),
                 ('import', 7, 'IMPORT_REJECTED: ECAD support disabled: other failure'),
                 ('import', 2, 'IMPORT_REJECTED: ECAD support disabled'),
                 ('preview', 7, 'IMPORT_REJECTED: ECAD support disabled'),
                 ('solve', 7, 'IMPORT_REJECTED: ECAD support disabled')]
        for name, code, message in cases:
            with self.subTest(name=name, code=code, message=message):
                with self.assertRaises(AssertionError) as error:
                    self.invoke(name, code, message)
                self.assertNotIsInstance(error.exception, DependencyMissing)
        self.assertEqual(self.invoke(message='IMPORT_REJECTED: C-tier defects',
                                     expected=7).returncode, 7)

    def test_ecad_dependency_skips_normally_but_fails_strict_acceptance(self):
        with patch.dict(os.environ, {'SP7_STRICT_ACCEPTANCE': '0'}):
            with self.assertRaises(SystemExit) as error:
                main(lambda directory: self.invoke(), needs_occt=False)
            self.assertEqual(error.exception.code, 77)
        with patch.dict(os.environ, {'SP7_STRICT_ACCEPTANCE': '1'}):
            with self.assertRaises(DependencyMissing):
                main(lambda directory: self.invoke(), needs_occt=False)


class FixtureMaterials(unittest.TestCase):
    def test_homogenized_mapping_uses_an_equivalent_silicon_card(self):
        with tempfile.TemporaryDirectory(prefix='sp7-materials-') as tmp:
            directory = Path(tmp)
            prepare(directory)
            mapping = json.loads((directory / 'map.json').read_text())
            cards = json.loads((directory / 'materials.json').read_text())['materials']
        self.assertEqual(mapping, {'Si': 'silicon_eq'})
        silicon = next(card for card in cards if card['id'] == 'silicon')
        equivalent = [card for card in cards if card['id'] == 'silicon_eq']
        self.assertEqual(len(equivalent), 1)
        self.assertEqual(equivalent[0], dict(silicon, id='silicon_eq'))


if __name__ == '__main__':
    unittest.main(argv=[__file__])
