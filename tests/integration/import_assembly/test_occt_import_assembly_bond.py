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
        # Resolve attachment faces from the unmodified board, before merge can
        # introduce roundoff into the exported layer thicknesses.
        check(baseline.run('--accept-b-tier'), 0)
        board_layers = list(intervals(baseline.stack()))
        cls.board_top = next(z1 for _, z1, layer in board_layers if layer['name'] == 'B.Cu')
        cls.board_bottom_name = board_layers[0][2]['name']
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
        cls.lid_height = sum(layer['thickness'] for _, _, layer in layers
                             if has_body(layer, 'lid'))
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

    def close(self, actual, expected, *, scale):
        tolerance = 4 * sys.float_info.epsilon * scale
        self.assertLessEqual(abs(actual - expected), tolerance, (actual, expected))

    def place(self, thickness, *, bottom=False, bond=True):
        attach = (f'layer:{self.board_bottom_name}@bottom' if bottom
                  else 'layer:B.Cu@top')
        placement = {'attach': attach, 'gap': thickness}
        if bond:
            placement['bond'] = {'material': 'fr4_eq', 'thickness': thickness}
        self.a.document['parts'][0]['placement'] = placement
        self.a.save()

    def assert_bond(self, stack, lower, upper, *, face, origin=0.0):
        # Both bond faces use the attachment rule, independent of stack origin
        # and the absolute coordinates of the compared endpoints.
        scale = max(abs(self.thickness), abs(face))
        layers = list(intervals(stack))
        bonded = [(z0 - origin, z1 - origin, layer) for z0, z1, layer in layers
                  if has_body(layer, 'lid.bond')]
        self.assertTrue(bonded, 'missing lid.bond regions: attach bond gap is still void')
        self.close(min(z0 for z0, _, _ in bonded), lower, scale=scale)
        self.close(max(z1 for _, z1, _ in bonded), upper, scale=scale)
        for z0, z1, layer in bonded:
            self.close(z0, lower, scale=scale)
            self.close(z1, upper, scale=scale)
            self.close(layer['thickness'], self.thickness, scale=self.thickness)
            # No background material: space outside the bond footprint stays void.
            self.assertFalse(layer.get('material'), layer)
            for region in layer['regions']:
                self.assertEqual(region['body'], 'lid.bond')
                self.assertEqual(region['material'], 'fr4_eq')
                self.assertTrue(region['name'].startswith('lid.bond#'), region)
                box = region['footprint']['box']
                self.assertEqual(len(box), len(self.lid_box))
                for actual, expected in zip(box, self.lid_box):
                    self.close(actual, expected, scale=abs(expected))
        # Check all layers intersecting the gap, not just the bond-bearing subset.
        gap = [(z0 - origin, z1 - origin, layer) for z0, z1, layer in layers
               if overlaps(z0 - origin, z1 - origin, lower, upper)]
        self.assertTrue(gap)
        self.close(gap[0][0], lower, scale=scale)
        self.close(gap[-1][1], upper, scale=scale)
        for _, _, layer in gap:
            self.assertTrue(has_body(layer, 'lid.bond'), layer)
        self.assertFalse([d for d in self.a.import_report()['defects']
                          if d['defect_id'].startswith('lid.bond/')])

    def test_top_bond_fills_gap(self):
        self.place(self.thickness)
        check(self.a.run('--accept-b-tier'), 0)
        stack = self.a.stack()
        self.assert_bond(stack, self.board_top, self.board_top + self.thickness,
                         face=self.board_top)
        self.close(min(z0 for z0, _, layer in intervals(stack) if has_body(layer, 'lid')),
                   self.board_top + self.thickness,
                   scale=max(self.thickness, abs(self.board_top)))

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
        scale = max(self.thickness, abs(self.board_top))
        self.close(min(z0 for z0, _, _ in bonded), self.board_top, scale=scale)
        self.close(max(z1 for _, z1, _ in bonded), self.board_top + self.thickness, scale=scale)
        for _, _, layer in bonded:
            self.close(layer['thickness'], self.thickness, scale=self.thickness)
            for region in layer['regions']:
                if region['body'] != 'lid.bond':
                    continue
                self.assertEqual(region['material'], 'fr4_eq')
                self.assertTrue(region['name'].startswith('lid.bond#'), region)
                box = region['footprint']['box']
                self.assertEqual(len(box), len(self.lid_box))
                for actual, expected in zip(box, self.lid_box):
                    self.close(actual, expected, scale=abs(expected))
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
        self.assert_bond(stack, -self.thickness, 0.0, face=0.0, origin=origin)
        self.close(max(z1 - origin for _, z1, layer in intervals(stack)
                       if has_body(layer, 'lid')), -self.thickness, scale=self.thickness)

    def test_bottom_bond_roundoff_meets_attachment_face(self):
        # Original roundoff witness from the architect's ruling: adding T to
        # this placed face used to create a spurious lid.bond/thin#1 interval.
        thickness = 7.000000000000001e-05
        placed_z1 = -6.999999999999997e-05
        self.close(self.thickness, thickness, scale=thickness)
        # place() translates the local top by dz = -gap - local_top.
        # Derive the face from the measured fixture height and configured gap,
        # then verify the CLI actually preserves that face in its output.
        local_top = self.lid_height
        predicted_z1 = local_top + (-thickness - local_top)
        self.close(predicted_z1, placed_z1, scale=abs(placed_z1))
        self.assertGreater(predicted_z1 + thickness, 0.0)
        self.close(predicted_z1 + thickness, 4.0657581468206416e-20,
                   scale=4.0657581468206416e-20)
        self.place(thickness, bottom=True)
        check(self.a.run(), 0)
        stack = self.a.stack()
        origin = next(z0 for z0, _, layer in intervals(stack)
                      if layer['name'] == self.board_bottom_name)
        actual_z1 = max(z1 - origin for _, z1, layer in intervals(stack)
                        if has_body(layer, 'lid'))
        self.close(actual_z1, predicted_z1, scale=abs(predicted_z1))
        self.assertGreater(actual_z1 + thickness, 0.0)
        self.close(actual_z1 + thickness, 4.0657581468206416e-20,
                   scale=4.0657581468206416e-20)
        self.assert_bond(stack, -thickness, 0.0, face=0.0, origin=origin)
        bond_top = max(z1 for _, z1, layer in intervals(stack)
                       if has_body(layer, 'lid.bond'))
        # Exact shared interface, with no absolute tolerance at the zero face.
        self.close(bond_top - origin, 0.0, scale=0.0)

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
        # Architect ruling: preserve main's schema rejection and publication
        # behavior. 整层空腔待卡 assembly-floating-gap.
        result = self.a.run('--accept-b-tier')
        check(result, 14)
        self.assertFalse(self.a.output.exists())
        self.assertEqual(result.stdout, '')
        self.assertEqual(result.stderr,
                         'INPUT_INVALID: Must be valid against at least one schema, '
                         'but found no matching schemas\n')
        self.assertEqual(self.a.defects()['code'], 'INPUT_INVALID')
        self.assertFalse([d for d in self.a.import_report()['defects']
                          if d['defect_id'].startswith('lid.bond/')])


if __name__ == '__main__':
    unittest.main(argv=[sys.argv[0]], verbosity=2)
