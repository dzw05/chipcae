# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
from pathlib import Path
import tempfile
import unittest
from assembly_fixtures import LINKS, add_assembly, fixture, h5py, invalid


def hard_link(root, assembly):
    try:
        assembly['volume/cut'] = root['cut']
    except (OSError, RuntimeError, ValueError) as exc:
        print(f'SKIP: cannot create hard link at Assembly/volume/cut: {exc}')
        raise SystemExit(77)


class Links(unittest.TestCase):
    def test_partial_assembly_reports_missing_category(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp) / 'run'
            step, _ = fixture(run)
            with h5py.File(step, 'r+') as f:
                assembly = add_assembly(f['VTKHDF'])
                del assembly['boundary']
                del assembly['lagrangian']
                del assembly['volume/cut']
            invalid(self, run, 'VTKHDF/Assembly', 'missing category', 'boundary')

    def test_missing_renamed_group_dataset_hard_and_external_links(self):
        for case, path in [
                ('missing', 'volume/hex'), ('renamed', 'volume/hex'),
                ('group', 'volume/cut'), ('dataset', 'volume/cut'),
                ('hard', 'volume/cut'), ('external', 'volume/cut')]:
            with self.subTest(case=case), tempfile.TemporaryDirectory() as tmp:
                run = Path(tmp) / 'run'
                step, _ = fixture(run)
                with h5py.File(step, 'r+') as f:
                    root = f['VTKHDF']
                    assembly = add_assembly(root)
                    if case == 'renamed':
                        assembly.move(path, 'volume/Hex')
                    else:
                        del assembly[path]
                        if case == 'group':
                            assembly.create_group(path)
                        elif case == 'dataset':
                            assembly[path] = [1]
                        elif case == 'hard':
                            hard_link(root, assembly)
                            self.assertIsInstance(assembly.get(path, getlink=True), h5py.HardLink)
                        elif case == 'external':
                            assembly[path] = h5py.ExternalLink('missing.vtkhdf', '/VTKHDF/cut')
                invalid(self, run, 'VTKHDF/Assembly/' + path, 'expected soft link')

    def test_targets_require_literal_case_and_no_trailing_slash(self):
        cases = [
            ('boundary/eb_surface', '/VTKHDF/hex', '/VTKHDF/eb_surface'),
            ('boundary/eb_surface', '/vtkhdf/eb_surface', '/VTKHDF/eb_surface'),
            ('volume/hex', '/vtkhdf/hex', '/VTKHDF/hex'),
            ('volume/hex', '/VTKHDF/hex/', '/VTKHDF/hex'),
        ]
        for path, target, expected in cases:
            with self.subTest(path=path, target=target), tempfile.TemporaryDirectory() as tmp:
                run = Path(tmp) / 'run'
                step, _ = fixture(run)
                # HDF5 normalizes trailing slashes during link creation. Store a
                # same-length sentinel, then patch the legacy, unchecksummed heap
                # after closing the file so this case really has the bad target.
                stored_target = target[:-1] + '!' if target.endswith('/') else target
                with h5py.File(step, 'r+') as f:
                    assembly = add_assembly(f['VTKHDF'])
                    del assembly[path]
                    assembly[path] = h5py.SoftLink(stored_target)
                    if target.endswith('/'):
                        self.assertEqual(h5py.h5o.get_info(assembly['volume'].id).hdr.version, 1)
                if target.endswith('/'):
                    data = step.read_bytes()
                    marker = stored_target.encode('ascii') + b'\0'
                    self.assertEqual(data.count(marker), 1)
                    step.write_bytes(data.replace(marker, target.encode('ascii') + b'\0'))
                with h5py.File(step, 'r') as f:
                    link = f['VTKHDF/Assembly'].get(path, getlink=True)
                    self.assertIsInstance(link, h5py.SoftLink)
                    self.assertEqual(link.path, target)
                invalid(self, run, 'VTKHDF/Assembly/' + path, 'expected ' + expected)

    def test_deleted_particles_target_is_dangling(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp) / 'run'
            step, _ = fixture(run)
            with h5py.File(step, 'r+') as f:
                assembly = add_assembly(f['VTKHDF'])
                del f['VTKHDF/particles']
                link = assembly.get('lagrangian/particles', getlink=True)
                self.assertIsInstance(link, h5py.SoftLink)
                self.assertEqual(link.path, '/VTKHDF/particles')
            invalid(self, run, 'VTKHDF/Assembly/lagrangian/particles', 'dangling soft link')

    def test_link_failure_precedence_follows_table_order(self):
        # Isolated acceptance cases above; these deliberately combine failures.
        for first in range(len(LINKS)):
            with self.subTest(first=LINKS[first][0]), tempfile.TemporaryDirectory() as tmp:
                run = Path(tmp) / 'run'
                step, _ = fixture(run)
                with h5py.File(step, 'r+') as f:
                    assembly = add_assembly(f['VTKHDF'])
                    for path, _ in reversed(LINKS[first:]):
                        del assembly[path]
                    if first == 0:
                        # Wrong (and dangling) target wins before later missing links.
                        assembly['volume/hex'] = h5py.SoftLink('/wrong/hex')
                path, target = LINKS[first]
                message = 'expected ' + target if first == 0 else 'expected soft link'
                invalid(self, run, 'VTKHDF/Assembly/' + path, message)


if __name__ == '__main__':
    unittest.main(argv=[__file__])
