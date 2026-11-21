"""CLI acceptance: attach bonds fill the gap and participate in thin-layer review."""
from pathlib import Path
import math
import shutil
import sys
import tempfile
import unittest

from assembly_test_support import (
    Assembly, b_ids, check, require, require_assembly_geometry,
)

require('JSONCONS')
require('ECAD')
require_assembly_geometry()


def intervals(stack):
    z = 0.0
    for layer in stack['layers']:
        end = z + layer['thickness']
        yield z, end, layer
        z = end


def has_body(layer, body):
    return any(r.get('body') == body for r in layer.get('regions', []))


def overlaps(z0, z1, lower, upper):
    # Board-relative subtraction can move a shared face by a few ULPs.
    return (z0 < upper and z1 > lower
            and not math.isclose(z0, upper, rel_tol=4 * sys.float_info.epsilon, abs_tol=0)
            and not math.isclose(z1, lower, rel_tol=4 * sys.float_info.epsilon, abs_tol=0))


class AssemblyBond(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.baseline_dir = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.baseline_dir.cleanup)
        baseline = Assembly(cls.baseline_dir.name)
        baseline.boxes()
        check(baseline.run('--accept-b-tier'), 0)
        stack = baseline.stack()
        cls.t_min = baseline.import_report()['t_min']
        cls.eps_weld = baseline.import_report()['eps_weld']
        cls.thickness = 2 * cls.t_min
        layers = list(intervals(stack))
        first_lid = next(i for i, (_, _, layer) in enumerate(layers)
                         if has_body(layer, 'lid'))
        assert first_lid > 0, stack
        cls.board_top = sum(layer['thickness'] for _, _, layer in layers[:first_lid])
        cls.board_bottom_name = layers[0][2]['name']
        cls.lid_box = next(r['footprint']['box']
                           for r in layers[first_lid][2]['regions'] if r['body'] == 'lid')
        assert not any(has_body(layer, 'lid.bond') for _, _, layer in layers), stack

    def setUp(self):
        # Every case starts without an output stack, especially the exit-6 case.
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.a = Assembly(directory.name)
        shutil.copyfile(Path(self.baseline_dir.name) / 'lid.step', self.a.root / 'lid.step')
        self.a.part()

    def close(self, actual, expected):
        tolerance = 4 * sys.float_info.epsilon * max(abs(actual), abs(expected))
        self.assertLessEqual(abs(actual - expected), tolerance, (actual, expected))

    def place(self, thickness, *, bottom=False, bond=True):
        attach = (f'layer:{self.board_bottom_name}@bottom' if bottom
                  else 'layer:B.Cu@top')
        placement = {'attach': attach, 'gap': thickness}
        if bond:
            placement['bond'] = {'material': 'fr4_eq', 'thickness': thickness}
        self.a.document['parts'][0]['placement'] = placement
        self.a.save()

    def assert_bond(self, stack, lower, upper, *, origin=0.0):
        layers = list(intervals(stack))
        bonded = [(z0 - origin, z1 - origin, layer) for z0, z1, layer in layers
                  if has_body(layer, 'lid.bond')]
        self.assertTrue(bonded, 'missing lid.bond regions: attach bond gap is still void')
        self.close(min(z0 for z0, _, _ in bonded), lower)
        self.close(max(z1 for _, z1, _ in bonded), upper)
        for z0, z1, layer in bonded:
            self.close(z0, lower)
            self.close(z1, upper)
            self.close(layer['thickness'], self.thickness)
            # No background material: space outside the bond footprint stays void.
            self.assertFalse(layer.get('material'), layer)
            for region in layer['regions']:
                self.assertEqual(region['body'], 'lid.bond')
                self.assertEqual(region['material'], 'fr4_eq')
                self.assertTrue(region['name'].startswith('lid.bond#'), region)
                box = region['footprint']['box']
                self.assertEqual(len(box), len(self.lid_box))
                for actual, expected in zip(box, self.lid_box):
                    self.close(actual, expected)
        # Check all layers intersecting the gap, not just the bond-bearing subset.
        gap = [(z0 - origin, z1 - origin, layer) for z0, z1, layer in layers
               if overlaps(z0 - origin, z1 - origin, lower, upper)]
        self.assertTrue(gap)
        self.close(gap[0][0], lower)
        self.close(gap[-1][1], upper)
        for _, _, layer in gap:
            self.assertTrue(has_body(layer, 'lid.bond'), layer)
        self.assertFalse([d for d in self.a.import_report()['defects']
                          if d['defect_id'].startswith('lid.bond/')])

    def test_top_bond_fills_gap(self):
        self.place(self.thickness)
        check(self.a.run('--accept-b-tier'), 0)
        stack = self.a.stack()
        self.assert_bond(stack, self.board_top, self.board_top + self.thickness)
        self.close(min(z0 for z0, _, layer in intervals(stack) if has_body(layer, 'lid')),
                   self.board_top + self.thickness)

    def add_frame_witness(self):
        self.a.part('witness', 'lid.step',
                    placement={'frame': 'step', 'origin': [.003, 0, self.board_top]})

    def test_top_bond_alongside_frame_part(self):
        self.place(self.thickness)
        self.a.document['parts'][0]['material_approx'] = True
        # The disjoint frame part gives every gap layer a region. This isolates
        # missing bond geometry from the existing schema rejection of void layers.
        self.add_frame_witness()
        check(self.a.run('--accept-b-tier'), 0)
        stack = self.a.stack()
        self.assertTrue(any(has_body(layer, 'witness') for layer in stack['layers']))
        self.assertTrue(any(has_body(layer, 'lid.bond') for layer in stack['layers']),
                        'missing lid.bond regions: attach bond gap is still void')
        bonded = [(z0, z1, layer) for z0, z1, layer in intervals(stack)
                  if has_body(layer, 'lid.bond')]
        self.close(min(z0 for z0, _, _ in bonded), self.board_top)
        self.close(max(z1 for _, z1, _ in bonded), self.board_top + self.thickness)
        for _, _, layer in bonded:
            self.close(layer['thickness'], self.thickness)
            for region in layer['regions']:
                if region['body'] != 'lid.bond':
                    continue
                self.assertEqual(region['material'], 'fr4_eq')
                self.assertTrue(region['name'].startswith('lid.bond#'), region)
                box = region['footprint']['box']
                self.assertEqual(len(box), len(self.lid_box))
                for actual, expected in zip(box, self.lid_box):
                    self.close(actual, expected)
        # Approximation belongs to the input lid, never its synthetic bond.
        self.assertEqual(self.a.import_report()['parts']['material_approx'],
                         [{'id': 'lid', 'material_approx': True, 'label': '近似材料'}])

    def test_bottom_bond_fills_gap(self):
        self.place(self.thickness, bottom=True)
        check(self.a.run('--accept-b-tier'), 0)
        stack = self.a.stack()
        # Stack JSON has thicknesses only; anchor negative z to the board's lower face.
        origin = next(z0 for z0, _, layer in intervals(stack)
                      if layer['name'] == self.board_bottom_name)
        self.assert_bond(stack, -self.thickness, 0.0, origin=origin)
        self.close(max(z1 - origin for _, z1, layer in intervals(stack)
                       if has_body(layer, 'lid')), -self.thickness)

    def thin_bond(self):
        thickness = (self.eps_weld + self.t_min) / 2
        self.assertLess(self.eps_weld, thickness)
        self.assertLess(thickness, self.t_min)
        self.place(thickness)

    def assert_thin_bond_report(self):
        report = self.a.import_report()
        matching = [id_ for id_ in b_ids(report) if id_.startswith('lid.bond/thin#')]
        self.assertTrue(matching, report)
        for defect in report['defects']:
            if defect['defect_id'] in matching:
                self.assertEqual(defect['tier'], 'B')
                self.assertIs(type(defect['reason']), int)
                self.assertEqual(defect['reason'], 1)

    def test_thin_bond_requires_acceptance(self):
        self.thin_bond()
        check(self.a.run(), 6)
        self.assertFalse(self.a.output.exists())
        self.assert_thin_bond_report()

    def test_thin_bond_alongside_frame_part_requires_acceptance(self):
        self.thin_bond()
        self.add_frame_witness()
        check(self.a.run(), 6)
        self.assertFalse(self.a.output.exists())
        self.assert_thin_bond_report()

    def test_thin_bond_can_be_accepted(self):
        self.thin_bond()
        check(self.a.run('--accept-b-tier'), 0)
        self.a.stack()

    def test_gap_without_bond_stays_void(self):
        self.place(self.thickness, bond=False)
        check(self.a.run('--accept-b-tier'), 0)
        stack = self.a.stack()
        layers = list(intervals(stack))
        self.assertFalse(any(has_body(layer, 'lid.bond') for _, _, layer in layers))
        gap = [(z0, z1, layer) for z0, z1, layer in layers
               if overlaps(z0, z1, self.board_top, self.board_top + self.thickness)]
        self.assertTrue(gap, stack)
        self.close(gap[0][0], self.board_top)
        self.close(gap[-1][1], self.board_top + self.thickness)
        self.close(sum(layer['thickness'] for _, _, layer in gap), self.thickness)
        for _, _, layer in gap:
            self.assertFalse(layer.get('material'), layer)
            self.assertFalse(layer.get('regions'), layer)


if __name__ == '__main__':
    unittest.main(argv=[sys.argv[0]], verbosity=2)
