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
                        left, right = target[len('interface:'):].split('|')
                        self.assertEqual(layers.index(right), layers.index(left) + 1)

    def test_duplicate_id_reports_and_error_precedence(self):
        # Literal outcomes for baseline inputs and error-precedence regressions.
        cases = (
            ('pair', ['lid', 'lid'], None, True,
             7, 'IMPORT_REJECTED', 'IMPORT_REJECTED: duplicate part id lid',
             {('lid/duplicate_id', 'C', 7, 'duplicate part id lid')},
             {('lid/duplicate_id', 'C', 7)}),
            ('triple', ['lid', 'lid', 'lid'], None, True,
             7, 'IMPORT_REJECTED', 'IMPORT_REJECTED: duplicate part id lid',
             {('lid/duplicate_id', 'C', 7, 'duplicate part id lid')},
             {('lid/duplicate_id', 'C', 7)}),
            ('two_ids', ['lid', 'x', 'lid', 'x'], None, True,
             7, 'IMPORT_REJECTED', 'IMPORT_REJECTED: duplicate part id lid',
             {('lid/duplicate_id', 'C', 7, 'duplicate part id lid'),
              ('x/duplicate_id', 'C', 7, 'duplicate part id x')},
             {('lid/duplicate_id', 'C', 7), ('x/duplicate_id', 'C', 7)}),
            ('duplicate_before_void', ['lid', 'lid', 'void'], None, True,
             7, 'IMPORT_REJECTED', 'IMPORT_REJECTED: duplicate part id lid',
             {('lid/duplicate_id', 'C', 7, 'duplicate part id lid')},
             {('lid/duplicate_id', 'C', 7)}),
            ('void_before_duplicate', ['void', 'lid', 'lid'], None, True,
             14, 'INPUT_INVALID', 'INPUT_INVALID: part id void is reserved',
             set(), set()),
            ('explicit_and_body', ['lid'], ['lid'], True,
             7, 'IMPORT_REJECTED', 'IMPORT_REJECTED: duplicate part id lid',
             {('lid/duplicate_id', 'C', 7, 'duplicate part id lid')},
             {('lid/duplicate_id', 'C', 7)}),
            ('two_bodies', [], ['a', 'a'], True,
             7, 'IMPORT_REJECTED', 'IMPORT_REJECTED: duplicate part id a',
             {('a/duplicate_id', 'C', 7, 'duplicate part id a')},
             {('a/duplicate_id', 'C', 7)}),
            ('without_confirmation', ['lid', 'lid'], None, False,
             7, 'IMPORT_REJECTED', 'IMPORT_REJECTED: duplicate part id lid',
             {('lid/duplicate_id', 'C', 7, 'duplicate part id lid')},
             {('lid/duplicate_id', 'C', 7)}),
            ('duplicate_before_display', ['lid', 'lid', 'display'], None, True,
             7, 'IMPORT_REJECTED', 'IMPORT_REJECTED: duplicate part id lid',
             {('lid/duplicate_id', 'C', 7, 'duplicate part id lid')},
             {('lid/duplicate_id', 'C', 7)}),
            ('display_before_duplicate', ['display', 'lid', 'lid'], None, True,
             7, 'IMPORT_REJECTED',
             'IMPORT_REJECTED: display-only geometry NOT_CAE_GEOMETRY',
             set(), set()),
            ('body_duplicate_before_void', [], ['a', 'a', 'void'], True,
             14, 'INPUT_INVALID', 'INPUT_INVALID: part id void is reserved',
             set(), set()),
            ('body_void_before_duplicate', [], ['void', 'a', 'a'], True,
             14, 'INPUT_INVALID', 'INPUT_INVALID: part id void is reserved',
             set(), set()),
            ('body_duplicate_before_display', [], ['a', 'a'], True,
             7, 'IMPORT_REJECTED', 'IMPORT_REJECTED: duplicate part id a',
             {('a/duplicate_id', 'C', 7, 'duplicate part id a')},
             {('a/duplicate_id', 'C', 7)}),
        )
        for (name, ids, body_ids, accept, exit_code, code, message,
             expected_import, expected_defects) in cases:
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
                if name == 'body_duplicate_before_display':
                    a.document['parts_from_step']['step'] = 'NOT_CAE_GEOMETRY.step'
                a.save()
                result = a.run(*(['--accept-b-tier'] if accept else []))
                check(result, exit_code)
                self.assertEqual(a.defects()['code'], code)
                self.assertEqual(a.defects()['message'], message)
                self.assertFalse(a.output.exists())
                for report_name, report in (('import_report', a.import_report()),
                                            ('defects', a.defects())):
                    with self.subTest(report=report_name):
                        defects = [d for d in report['defects']
                                   if not d['defect_id'].startswith('board/')]
                        if report_name == 'import_report':
                            self.assertEqual(len(defects), len(expected_import))
                            self.assertEqual({(d['defect_id'], d['tier'], d['reason'],
                                               d['details']) for d in defects}, expected_import)
                        else:
                            self.assertEqual(len(defects), len(expected_defects))
                            self.assertEqual({(d['defect_id'], d['tier'], d['reason'])
                                              for d in defects},
                                             expected_defects)
                            for defect in defects:
                                self.assertNotIn('details', defect)
                        for defect in defects:
                            self.assertEqual(defect['kind'], '')


if __name__ == '__main__':
    unittest.main(argv=[__file__], verbosity=2)
