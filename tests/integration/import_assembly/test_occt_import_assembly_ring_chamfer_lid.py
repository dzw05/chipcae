"""Synthetic ring/lid acceptance: exact board coincidence, topology and identity."""
import itertools
import math
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

from assembly_test_support import (
    Assembly, SOURCE, check, require, require_assembly_geometry, step_generator,
)

require('JSONCONS')
require('ECAD')
require_assembly_geometry()


def protocol_error(name):
    entries = re.findall(r'\{code:\s*(\d+),\s*name:\s*([A-Z_]+),',
                         (SOURCE / 'protocol/errors.yaml').read_text())
    return next((label, int(code)) for code, label in entries if label == name)


REJECTED, REJECTED_EXIT = protocol_error('IMPORT_REJECTED')


def signed_area(points):
    return math.fsum(p[0] * q[1] - q[0] * p[1]
                     for p, q in zip(points, points[1:] + points[:1])) / 2


def canonical(points):
    first = min(range(len(points)), key=lambda i: tuple(points[i]))
    return points[first:] + points[:first]


class RingChamferLid(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.fixtures = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.fixtures.cleanup)
        cls.steps = Path(cls.fixtures.name)
        # Generate each variant once; repeat imports read the very same STEP bytes.
        for mode, extra in [('straight', []), ('rotated', ['rot']), ('normal', [])]:
            folder = cls.steps / mode
            folder.mkdir()
            generator_mode = 'normal' if mode == 'normal' else 'ring_chamfer_lid'
            check(subprocess.run([step_generator(), str(folder), generator_mode, *extra],
                                 capture_output=True, text=True, timeout=20), 0)

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.a = Assembly(self.directory.name)
        shutil.copytree(self.steps, self.a.root / 'steps')
        board = self.a.root / 'board.xml'
        before, profile = board.read_text().split('<Profile>', 1)
        _, after = profile.split('</Profile>', 1)
        # Change only the profile, retaining the three layers and copper features.
        board.write_text(before + '<Profile><Polygon><PolyBegin x="0" y="0"/>'
                         '<PolyStepSegment x="56" y="0"/>'
                         '<PolyStepSegment x="56" y="44"/>'
                         '<PolyStepSegment x="0" y="44"/>'
                         '</Polygon></Profile>' + after)

    def ring(self, variant='straight'):
        self.a.part('stiffener', f'steps/{variant}/stiffener_ring.step',
                    placement={'attach': 'layer:B.Cu@top', 'xy': [.008, .002], 'gap': 0})

    def lid(self, name='lid', attach='layer:B.Cu@top'):
        self.a.part(name, 'steps/straight/lid_chamfer.step',
                    placement={'attach': attach, 'xy': [0, 0], 'gap': 0})

    def success(self):
        result = self.a.run()
        check(result, 0)
        self.assertNotIn('C-tier', result.stderr)
        self.assertFalse([d for d in self.a.import_report()['defects'] if d['tier'] == 'C'])
        return self.a.stack()

    def close(self, actual, expected, *, absolute=0):
        self.assertTrue(math.isclose(actual, expected, rel_tol=1e-12, abs_tol=absolute),
                        (actual, expected))

    def base(self, stack, added):
        layers = stack['layers']
        self.assertEqual([l['name'] for l in layers],
                         ['F.Cu', 'Core', 'B.Cu'] + added)
        for layer, thickness, kind in zip(layers[:3], [35e-6, 1.6e-3, 35e-6],
                                          ['Patterned', 'Homogenized', 'Patterned']):
            self.close(layer['thickness'], thickness)
            self.assertEqual(layer['kind'], kind)
            self.assertEqual(layer['material'], 'fr4_eq')
            self.assertFalse(layer.get('regions'))
            if kind == 'Patterned':
                self.assertEqual(layer['copper'], {
                    'metal': 'copper', 'polygons': {
                        'file': self.a.output.with_suffix('.polyset').name,
                        'layer': layer['name']}})
            else:
                self.assertNotIn('copper', layer)
        for layer in layers[3:]:
            self.assertEqual(layer['kind'], 'Explicit')
            self.assertNotIn('material', layer)
            self.close(layer['thickness'], .0005)
        for actual, expected in zip(stack['outline']['box'], [0, 0, .056, .044]):
            self.close(actual, expected, absolute=1e-15)

    def z_interval(self, stack, index, expected):
        start = math.fsum(l['thickness'] for l in stack['layers'][:index])
        self.close(start, expected[0])
        self.close(start + stack['layers'][index]['thickness'], expected[1])

    def ring_geometry(self, layer):
        regions = layer['regions']
        self.assertEqual([r['name'] for r in regions],
                         [f'stiffener#0#{i}' for i in range(4)])
        for region in regions:
            self.assertEqual(region['body'], 'stiffener')
            self.assertEqual(region['material'], 'copper')
            self.assertFalse(region['footprint'].get('polygon'))
        boxes = sorted(r['footprint']['box'] for r in regions)
        expected = [[8, 2, 13, 42], [13, 2, 43, 7],
                    [13, 37, 43, 42], [43, 2, 48, 42]]
        for actual, want in zip(boxes, expected):
            for coordinate, mm in zip(actual, want):
                self.close(coordinate, mm * 1e-3, absolute=1e-9)
        for a, b in itertools.combinations(boxes, 2):
            area = max(0, min(a[2], b[2]) - max(a[0], b[0])) * max(
                0, min(a[3], b[3]) - max(a[1], b[1]))
            self.assertEqual(area, 0)
        self.close(math.fsum((b[2] - b[0]) * (b[3] - b[1]) for b in boxes), 700e-6)

    def lid_geometry(self, stack, layer, name='lid'):
        self.assertEqual(len(layer['regions']), 1)
        region = layer['regions'][0]
        self.assertEqual(region['name'], name + '#0#0')
        self.assertEqual(region['body'], name)
        self.assertEqual(region['material'], 'copper')
        footprint = region['footprint']
        outline = stack['outline']['box']
        self.assertEqual(footprint['box'], outline)  # Exact imported doubles, by contract.
        actual = footprint['polygon']
        self.assertEqual(len(actual), 8)
        self.assertGreater(signed_area(actual), 0)
        self.close(signed_area(actual), 2456e-6)
        # Independent contract vertices, in CCW order; only boundary values snap.
        expected = [[x * 1e-3, y * 1e-3] for x, y in
                    [(2, 0), (54, 0), (56, 2), (56, 42),
                     (54, 44), (2, 44), (0, 42), (0, 2)]]
        for point in expected:
            for axis in range(2):
                candidates = [side for side in (outline[axis], outline[axis + 2])
                              if abs(point[axis] - side) <= 1e-12]
                if candidates:
                    point[axis] = min(candidates, key=lambda side: abs(point[axis] - side))
        for point, want in zip(canonical(actual), canonical(expected)):
            for axis in range(2):
                self.assertLessEqual(abs(point[axis] - want[axis]), 1e-9)
                if want[axis] in (outline[axis], outline[axis + 2]):
                    self.assertEqual(point[axis], want[axis])
        self.assertEqual(footprint['box'], [min(p[0] for p in actual),
                                          min(p[1] for p in actual),
                                          max(p[0] for p in actual),
                                          max(p[1] for p in actual)])

    def test_ring_and_chamfer_lid(self):
        self.ring()
        self.lid(attach='part:stiffener@top')
        stack = self.success()
        self.base(stack, ['assembly#3', 'assembly#4'])
        self.z_interval(stack, 3, [.00167, .00217])
        self.z_interval(stack, 4, [.00217, .00267])
        self.ring_geometry(stack['layers'][3])
        self.lid_geometry(stack, stack['layers'][4])

    def test_repeat_import_is_byte_identical(self):
        self.ring()
        self.lid(attach='part:stiffener@top')
        self.success()
        first = self.a.output.read_bytes()
        self.success()
        self.assertEqual(self.a.output.read_bytes(), first)

    def test_lid_only_coincides_with_outline(self):
        self.lid()
        stack = self.success()
        self.base(stack, ['assembly#3'])
        self.z_interval(stack, 3, [.00167, .00217])
        self.lid_geometry(stack, stack['layers'][3])

    def test_rotated_ring_is_rejected_without_publishing(self):
        self.ring('rotated')
        self.lid(attach='part:stiffener@top')
        result = self.a.run()
        check(result, REJECTED_EXIT)
        self.assertIn(REJECTED + ': C-tier defects', result.stderr)
        self.assertEqual(self.a.defects()['code'], REJECTED)
        defects = self.a.import_report()['defects']
        self.assertTrue(any(d['tier'] == 'C' and
                            d['defect_id'].startswith('stiffener/cut_unsupported#') and
                            'holed non-rectilinear section' in d.get('details', '')
                            for d in defects), defects)
        self.assertFalse(self.a.output.exists())

    def test_part_ids_preserve_case(self):
        self.lid()
        self.lid('Lid', 'part:lid@top')
        stack = self.success()
        self.base(stack, ['assembly#3', 'assembly#4'])
        for index, name, interval in [(3, 'lid', [.00167, .00217]),
                                       (4, 'Lid', [.00217, .00267])]:
            self.z_interval(stack, index, interval)
            self.lid_geometry(stack, stack['layers'][index], name)

    def test_attachment_target_is_case_sensitive(self):
        self.lid()
        self.lid('second', 'part:LID@top')
        check(self.a.run(), REJECTED_EXIT)
        self.assertEqual(self.a.defects()['code'], REJECTED)
        self.assertIn('missing attach target part:LID', self.a.defects()['message'])
        self.assertFalse(self.a.output.exists())

    def test_step_body_exact_case_succeeds(self):
        self.a.part('lid', 'steps/normal/bulk.step', body='lid')
        stack = self.success()
        self.base(stack, ['assembly#3'])
        regions = stack['layers'][3]['regions']
        self.assertEqual([(r['name'], r['body']) for r in regions], [('lid#0#0', 'lid')])

    def test_step_body_wrong_case_is_unknown(self):
        self.a.part('lid', 'steps/normal/bulk.step', body='LID')
        check(self.a.run(), REJECTED_EXIT)
        self.assertEqual(self.a.defects()['code'], REJECTED)
        self.assertEqual(self.a.defects()['message'],
                         REJECTED + ': unknown body name LID')
        self.assertFalse(self.a.output.exists())


if __name__ == '__main__':
    # Auto-discovery passes the repository path as argv[1].
    unittest.main(argv=[__file__], verbosity=2)
