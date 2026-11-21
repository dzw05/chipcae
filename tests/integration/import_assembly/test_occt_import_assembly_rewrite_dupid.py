import copy
import tempfile
import unittest

from assembly_test_support import Assembly, check, require, require_assembly_geometry

require('JSONCONS')
require('ECAD')
require_assembly_geometry()


class AssemblyRewriteDuplicateIdTest(unittest.TestCase):
    def test_interface_targets_are_adjacent(self):
        with tempfile.TemporaryDirectory() as tmp:
            a = Assembly(tmp)
            a.boxes()
            board = a.root / 'board.xml'
            board.write_text(board.read_text().replace('Core', 'L'))
            a.document['parts'][0]['placement'] = {
                'frame': 'step', 'origin': [0, 0, .000035]}
            a.document['parts'][0]['material'] = 'fr4_eq'
            a.save()
            check(a.run('--accept-b-tier'), 0)
            targets = a.import_report()['parts']['rewrite_targets']
            for expected in (
                    {'from': 'interface:F.Cu|L', 'to': ['interface:F.Cu|L#0']},
                    {'from': 'interface:L|B.Cu', 'to': ['interface:L#1|B.Cu']},
                    {'from': 'layer:L', 'to': ['layer:L#0', 'layer:L#1']}):
                with self.subTest(expected=expected):
                    self.assertIn(expected, targets)
            layers = [layer['name'] for layer in a.stack()['layers']]
            for rewrite in targets:
                if rewrite['from'].startswith('interface:'):
                    with self.subTest(rewrite=rewrite):
                        self.assertEqual(len(rewrite['to']), 1)
                        target = rewrite['to'][0]
                        self.assertTrue(target.startswith('interface:'))
                        left, right = target.removeprefix('interface:').split('|')
                        self.assertEqual(layers.index(right), layers.index(left) + 1)

    def test_duplicate_id_reports_and_error_precedence(self):
        # Body keys sort in this order too, so precedence does not depend on JSON
        # object insertion order. Values are the part ids used by the CLI scan.
        cases = (
            ('pair', ['lid', 'lid'], None, True, 'lid'),
            ('triple', ['lid', 'lid', 'lid'], None, True, 'lid'),
            ('two_ids', ['lid', 'x', 'lid', 'x'], None, True, 'lid', ('lid', 'x')),
            ('duplicate_before_void', ['lid', 'lid', 'void'], None, True, 'lid'),
            ('void_before_duplicate', ['void', 'lid', 'lid'], None, True, 'void'),
            ('explicit_and_body', ['lid'], ['lid'], True, 'lid'),
            ('two_bodies', [], ['a', 'a'], True, 'a'),
            ('without_confirmation', ['lid', 'lid'], None, False, 'lid'),
            ('duplicate_before_display', ['lid', 'lid', 'display'], None, True, 'lid'),
            ('display_before_duplicate', ['display', 'lid', 'lid'], None, True, 'display'),
            ('body_duplicate_before_void', [], ['a', 'a', 'void'], True, 'a'),
            ('body_void_before_duplicate', [], ['void', 'a', 'a'], True, 'void'),
        )
        for name, ids, body_ids, accept, first, *duplicates in cases:
            with self.subTest(case=name), tempfile.TemporaryDirectory() as tmp:
                a = Assembly(tmp)
                a.boxes()
                template = copy.deepcopy(a.document['parts'][0])
                a.document['parts'] = []
                for id_ in ids:
                    part = copy.deepcopy(template)
                    part['id'] = id_
                    if id_ == 'display':
                        part['step'] = 'NOT_CAE_GEOMETRY.step'
                    a.document['parts'].append(part)
                if body_ids is not None:
                    a.document['parts_from_step'] = {
                        'step': 'bulk.step', 'frame': 'step', 'origin': [0, 0, .00167],
                        'bodies': {f'body{i}': {'id': id_, 'material': 'copper'}
                                   for i, id_ in enumerate(body_ids)}}
                a.save()
                result = a.run(*(['--accept-b-tier'] if accept else []))
                reserved = first == 'void'
                display = first == 'display'
                code = 'INPUT_INVALID' if reserved else 'IMPORT_REJECTED'
                message = ('part id void is reserved' if reserved else
                           'display-only geometry NOT_CAE_GEOMETRY' if display else
                           'duplicate part id ' + first)
                check(result, 14 if reserved else 7)
                self.assertEqual(a.defects()['code'], code)
                self.assertEqual(a.defects()['message'], code + ': ' + message)
                self.assertFalse(a.output.exists())
                duplicate_ids = () if reserved or display else (
                    duplicates[0] if duplicates else (first,))
                expected = {(id_ + '/duplicate_id', 'C', 7, 'duplicate part id ' + id_)
                            for id_ in duplicate_ids}
                for report_name, report in (('import_report', a.import_report()),
                                            ('defects', a.defects())):
                    with self.subTest(report=report_name):
                        defects = [d for d in report['defects']
                                   if not d['defect_id'].startswith('board/')]
                        self.assertEqual(len(defects), len(expected))
                        self.assertEqual({(d['defect_id'], d['tier'], d['reason'],
                                           d.get('details')) for d in defects}, expected)
                        for defect in defects:
                            self.assertEqual(defect['kind'], '')


if __name__ == '__main__':
    unittest.main(argv=[__file__], verbosity=2)
