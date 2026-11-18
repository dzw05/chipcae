# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
from pathlib import Path
import tempfile
import unittest
from assembly_fixtures import add_assembly, fixture, h5py, invalid, success


class Structure(unittest.TestCase):
    def test_isolated_structure_errors(self):
        cases = [
            ('dataset Assembly', 'VTKHDF/Assembly', ('expected group',)),
            ('empty Assembly', 'VTKHDF/Assembly', ('missing category', 'volume')),
            ('missing boundary', 'VTKHDF/Assembly', ('missing category', 'boundary')),
            ('extra other', 'VTKHDF/Assembly', ('unexpected member', 'other')),
            ('dataset volume', 'VTKHDF/Assembly/volume', ('expected group',)),
            ('renamed Volume', 'VTKHDF/Assembly', ('missing category', 'volume')),
        ]
        for case, path, messages in cases:
            with self.subTest(case=case), tempfile.TemporaryDirectory() as tmp:
                run = Path(tmp) / 'run'
                step, _ = fixture(run)
                with h5py.File(step, 'r+') as f:
                    root = f['VTKHDF']
                    assembly = add_assembly(root)
                    if case == 'dataset Assembly':
                        del root['Assembly']
                        root['Assembly'] = [1]
                    elif case == 'empty Assembly':
                        for category in list(assembly):
                            del assembly[category]
                    elif case == 'missing boundary':
                        del assembly['boundary']
                    elif case == 'extra other':
                        assembly.create_group('other')
                    elif case == 'dataset volume':
                        del assembly['volume']
                        assembly['volume'] = [1]
                    else:
                        assembly.move('volume', 'Volume')
                invalid(self, run, path, *messages)

    def test_lowercase_top_level_assembly_is_ignored(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp) / 'run'
            step, _ = fixture(run)
            before = success(self, run)
            with h5py.File(step, 'r+') as f:
                # Deliberately invalid if mistaken for exact-name Assembly.
                f['VTKHDF/assembly'] = [1]
            success(self, run, expected=before)

    def test_structure_precedence_with_multiple_failures(self):
        cases = [
            ('missing before extra and type', 'VTKHDF/Assembly', 'missing category', 'volume'),
            ('extra before type', 'VTKHDF/Assembly', 'unexpected member', 'alpha'),
            ('types before links', 'VTKHDF/Assembly/volume', 'expected group', None),
            ('Assembly before block fields', 'VTKHDF/Assembly', 'expected group', None),
        ]
        for case, path, message, name in cases:
            with self.subTest(case=case), tempfile.TemporaryDirectory() as tmp:
                run = Path(tmp) / 'run'
                step, _ = fixture(run)
                with h5py.File(step, 'r+') as f:
                    root = f['VTKHDF']
                    assembly = add_assembly(root)
                    if case == 'missing before extra and type':
                        del assembly['volume']
                        del assembly['boundary']
                        del assembly['lagrangian']
                        assembly['lagrangian'] = [1]
                        assembly.create_group('other')
                    elif case == 'extra before type':
                        # Reverse creation order to check sorted unexpected names.
                        assembly.create_group('zeta')
                        assembly.create_group('alpha')
                        del assembly['volume']
                        assembly['volume'] = [1]
                    elif case == 'types before links':
                        for category in ('boundary', 'volume'):
                            del assembly[category]
                            assembly[category] = [1]
                        del assembly['lagrangian/particles']
                    else:
                        del root['Assembly']
                        root['Assembly'] = [1]
                        del root['hex'].attrs['Type']
                invalid(self, run, path, message, *([name] if name else []))


if __name__ == '__main__':
    unittest.main(argv=[__file__])
