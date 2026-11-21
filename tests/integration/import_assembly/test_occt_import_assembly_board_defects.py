"""Board diagnostics participate in assembly rejection and B-tier confirmation."""
import json
import subprocess
import tempfile
import unittest

from assembly_test_support import (
    Assembly, BUILD, b_ids, check, require, require_assembly_geometry,
)

require('JSONCONS')
require('ECAD')
require_assembly_geometry()


def replace_once(text, anchor, replacement):
    assert text.count(anchor) == 1, f'expected exactly one XML anchor: {anchor}'
    return text.replace(anchor, replacement, 1)


def tier_ids(report, tier):
    return {d['defect_id'] for d in report['defects'] if d['tier'] == tier}


def board_ids(report):
    return {d['defect_id'] for d in report['defects']
            if d['defect_id'].startswith('board/')}


def standalone(a, expected):
    # Independent directory prevents an ECAD report from masking a missing assembly report.
    root = a.root / 'ecad'
    root.mkdir()
    check(subprocess.run([
        str(BUILD / 'kernel/chipcae-import'), str(a.root / 'board.xml'),
        '-o', str(root / 'stack.json'), '--material-map', str(a.root / 'map.json'),
        '--accept-b-tier',
    ], capture_output=True, text=True, timeout=25), expected)
    return json.loads((root / 'import_report.json').read_text())


def fresh_run(a, *args):
    # A previous successful run must not leave an output that masks a rejection bug.
    a.output.unlink(missing_ok=True)
    (a.root / 'import_report.json').unlink(missing_ok=True)
    result = a.run(*args)
    assert (a.root / 'import_report.json').is_file(), 'run must write a fresh import report'
    return result


class BoardDefects(unittest.TestCase):
    def test_c_board_cannot_be_accepted(self):
        with tempfile.TemporaryDirectory() as tmp:
            a = Assembly(tmp)
            a.boxes(thin=True)
            path = a.root / 'board.xml'
            xml = replace_once(
                path.read_text(), '<Ecad>',
                '<Content><DictionaryStandard units="MILLIMETER">'
                '<EntryStandard id="C1"><Circle diameter="0.2"/></EntryStandard>'
                '</DictionaryStandard></Content><Ecad>')
            xml = replace_once(
                xml, '<LayerFeature layerRef="F.Cu"><Set>',
                '<LayerFeature layerRef="F.Cu">'
                '<Set><Location x="2" y="2"/><StandardPrimitiveRef id="C1"/></Set><Set>')
            path.write_text(xml)
            expected = tier_ids(standalone(a, 7), 'C')
            self.assertTrue(expected, 'fixture must produce C-tier geometry in ECAD import')
            self.assertTrue(all(id_.startswith('board/') for id_ in expected))
            for args in [(), ('--accept-b-tier',), ('--accept', a.accept(sorted(expected)))]:
                with self.subTest(args=args):
                    result = fresh_run(a, *args)
                    check(result, 7)
                    self.assertFalse(a.output.exists(), 'C rejection must not write a stack')
                    self.assertEqual(tier_ids(a.import_report(), 'C'), expected)

    def test_b_board_requires_its_own_confirmation(self):
        with tempfile.TemporaryDirectory() as tmp:
            a = Assembly(tmp)
            a.boxes(thin=True)
            # Same geometry before inserting metadata: acceptance must not change stack bytes.
            check(a.run('--accept-b-tier'), 0)
            a.stack()
            baseline_stack = a.output.read_bytes()
            self.assertFalse(board_ids(a.import_report()))
            path = a.root / 'board.xml'
            path.write_text(replace_once(path.read_text(), '<Ecad>', '<MysteryA/><Ecad>'))
            ecad_report = standalone(a, 0)
            expected = board_ids(ecad_report)
            self.assertTrue(expected, 'fixture must produce board B-tier metadata in ECAD import')
            self.assertEqual(expected, tier_ids(ecad_report, 'B'))
            self.assertFalse(tier_ids(ecad_report, 'C'))

            result = fresh_run(a)
            check(result, 6)
            self.assertFalse(a.output.exists())
            report = a.import_report()
            part_ids = sorted(set(b_ids(report)) - board_ids(report))
            self.assertTrue(part_ids, 'fixture must exercise part B and board B together')
            with self.subTest(mode='unconfirmed report'):
                self.assertFalse(tier_ids(report, 'C'))
                self.assertEqual(board_ids(report), expected)
                self.assertTrue(expected <= set(b_ids(report)))

            all_b = sorted(set(part_ids) | expected)
            for mode in ('all B flag', 'explicit all B', 'explicit parts only'):
                with self.subTest(mode=mode):
                    args = ('--accept-b-tier',) if mode == 'all B flag' else (
                        '--accept', a.accept(part_ids if mode == 'explicit parts only' else all_b))
                    result = fresh_run(a, *args)
                    check(result, 6 if mode == 'explicit parts only' else 0)
                    if mode == 'explicit parts only':
                        self.assertFalse(a.output.exists())
                    else:
                        a.stack()
                        self.assertEqual(a.output.read_bytes(), baseline_stack)
                    self.assertFalse(tier_ids(a.import_report(), 'C'))
                    self.assertEqual(board_ids(a.import_report()), expected)

    def test_pristine_board_has_no_board_defects(self):
        with tempfile.TemporaryDirectory() as tmp:
            a = Assembly(tmp)
            a.boxes()
            check(a.run('--accept-b-tier'), 0)
            a.stack()
            self.assertFalse(board_ids(a.import_report()))
            baseline_stack = a.output.read_bytes()
            check(fresh_run(a, '--accept-b-tier'), 0)
            a.stack()
            self.assertFalse(board_ids(a.import_report()))
            self.assertEqual(a.output.read_bytes(), baseline_stack)


if __name__ == '__main__':
    unittest.main(argv=[__file__], verbosity=2)
