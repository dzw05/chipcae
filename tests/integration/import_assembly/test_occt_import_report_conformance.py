"""Validate live ECAD and assembly reports against the import-report/1.0 contract."""
import json
import math
from pathlib import Path
import subprocess
import tempfile
import unittest

from assembly_test_support import (
    SOURCE, BUILD, Assembly, check, b_ids, require, require_assembly_geometry,
)

require('JSONCONS')
require('ECAD')
require_assembly_geometry()

TIERS = {'Weld', 'A', 'B', 'C'}
KEYS = set('defect_id tier kind reason dimension size size_unit suggestion action '
           'rejection_reason material_ambiguous'.split())


def expect(ok, where, rule, actual):
    assert ok, f'{where}: expected {rule}; actual {actual!r}'


def field(obj, key, predicate, rule, where):
    expect(key in obj, f'{where}.{key}', f'required field ({rule})', obj)
    value = obj[key]
    expect(predicate(value), f'{where}.{key}', rule, value)
    return value


def finite(value):
    return type(value) in (int, float) and math.isfinite(value)


def natural(value):
    return type(value) is int and value >= 0


def strings(value):
    return isinstance(value, list) and all(isinstance(v, str) for v in value)


def strict_load(path):
    def reject_constant(value):
        raise ValueError(f'non-JSON constant {value}')
    try:
        doc = json.loads(path.read_text(encoding='utf-8'), parse_constant=reject_constant)
    except (OSError, ValueError) as error:
        raise AssertionError(f'{path}: expected UTF-8 RFC 8259 JSON; actual {error}') from error
    expect(isinstance(doc, dict), path, 'object', doc)
    field(doc, 'schema_version', lambda v: v == 'import-report/1.0',
          'import-report/1.0', path)
    return doc


def check_defects(doc, path, details=False):
    defects = field(doc, 'defects', lambda v: isinstance(v, list), 'array', path)
    ids = []
    for i, defect in enumerate(defects):
        where = f'{path}.defects[{i}]'
        expect(isinstance(defect, dict), where, 'object', defect)
        expect(KEYS <= defect.keys() <= KEYS | ({'details'} if details else set()),
               where, f'exact keys {sorted(KEYS)}' + (' plus optional details' if details else ''),
               sorted(defect))
        ids.append(field(defect, 'defect_id', lambda v: isinstance(v, str) and bool(v),
                         'nonempty string', where))
        field(defect, 'tier', lambda v: isinstance(v, str) and v in TIERS, 'Weld/A/B/C', where)
        field(defect, 'reason', natural, 'nonnegative integer, excluding bool', where)
        field(defect, 'size', lambda v: v is None or finite(v), 'null or finite number', where)
        field(defect, 'size_unit', lambda v: isinstance(v, str) and v in {'1', 'm', 'm2', 'm3'},
              '1/m/m2/m3', where)
        for key in ('kind', 'dimension', 'suggestion', 'action', 'rejection_reason'):
            field(defect, key, lambda v: isinstance(v, str), 'string', where)
        field(defect, 'material_ambiguous', lambda v: type(v) is bool, 'boolean', where)
        if 'details' in defect:
            field(defect, 'details', lambda v: isinstance(v, str), 'string', where)
    expect(len(ids) == len(set(ids)), f'{path}.defects.defect_id', 'unique IDs', ids)
    return defects


def check_import_report(path, assembly):
    doc = strict_load(path)
    for key in ('L', 't_min', 'eps_weld', 'tau_A', 'tau_B', 'delta_f'):
        field(doc, key, finite, 'finite number, excluding bool (zero allowed)', path)
    defects = check_defects(doc, path, details=assembly)
    counts = field(doc, 'tier_counts', lambda v: isinstance(v, dict) and v.keys() == TIERS,
                   'object with exactly Weld/A/B/C keys', path)
    for tier in sorted(TIERS):
        count = field(counts, tier, natural, 'nonnegative integer, excluding bool',
                      f'{path}.tier_counts')
        expected = sum(d['tier'] == tier for d in defects)
        expect(count == expected, f'{path}.tier_counts.{tier}', f'defect count {expected}', count)
    field(doc, 'b_tier_accepted', lambda v: v is None or v == 'all', 'null or "all"', path)
    if assembly:
        field(doc, 'welded_interfaces', natural, 'nonnegative integer, excluding bool', path)
        parts = field(doc, 'parts', lambda v: isinstance(v, dict), 'object', path)
        splits = field(parts, 'split_layers', lambda v: isinstance(v, dict), 'object', f'{path}.parts')
        for key, value in splits.items():
            expect(strings(value), f'{path}.parts.split_layers.{key}', 'string array', value)
        for key, keys in (('rewrite_targets', {'from', 'to'}),
                          ('material_approx', {'id', 'material_approx', 'label'})):
            entries = field(parts, key, lambda v: isinstance(v, list), 'array', f'{path}.parts')
            for i, entry in enumerate(entries):
                where = f'{path}.parts.{key}[{i}]'
                expect(isinstance(entry, dict) and entry.keys() == keys, where,
                       f'object with exact keys {sorted(keys)}', entry)
                if key == 'rewrite_targets':
                    field(entry, 'from', lambda v: isinstance(v, str) and
                          v.startswith(('layer:', 'region:', 'interface:')), 'prefixed target string', where)
                    field(entry, 'to', strings, 'string array', where)
                else:
                    field(entry, 'id', lambda v: isinstance(v, str), 'string', where)
                    field(entry, 'material_approx', lambda v: v is True, 'true', where)
                    field(entry, 'label', lambda v: v == '近似材料', '近似材料', where)
    else:
        for key in ('parts', 'welded_interfaces'):
            expect(key not in doc, f'{path}.{key}', 'absent in ECAD', doc.get(key))
    return doc


def check_summary(path, assembly, rc):
    doc = strict_load(path)
    for key in ('code', 'message'):
        field(doc, key, lambda v: isinstance(v, str), 'string', path)
    defects = check_defects(doc, path)
    if not assembly:
        expect(defects == [], f'{path}.defects', 'empty ECAD array', defects)
        expect(rc != 14, path, 'exit 14 only for assembly', rc)
    codes = {0: 'OK', 6: 'IMPORT_CONFIRMATION_REQUIRED', 7: 'IMPORT_REJECTED',
             9: 'MATERIAL_MAPPING_MISSING', 14: 'INPUT_INVALID'}
    expect(doc['code'] == codes[rc], f'{path}.code', codes[rc], doc['code'])
    return doc


class LiveReportConformance(unittest.TestCase):
    def test_live_reports(self):
        cases = [('ecad_success', False, 0), ('ecad_unconfirmed', False, 6),
                 ('ecad_accepted', False, 0), ('ecad_missing_mapping', False, 9),
                 ('assembly_success', True, 0), ('assembly_unconfirmed', True, 6),
                 ('assembly_accepted', True, 0), ('assembly_c_board', True, 7),
                 ('assembly_reserved_id', True, 14)]
        for case, assembly, rc in cases:
            with self.subTest(case=case), tempfile.TemporaryDirectory(prefix=case) as tmp:
                root = Path(tmp).resolve()
                if assembly:
                    a = Assembly(root)
                    if rc == 14:
                        (root / 'lid.step').write_text('reserved identifiers must be rejected before STEP import')
                        a.part('void')
                    else:
                        a.boxes(thin=case in ('assembly_unconfirmed', 'assembly_accepted', 'assembly_c_board'))
                    if rc == 7:
                        path = root / 'board.xml'
                        xml = path.read_text(encoding='utf-8')
                        for anchor, replacement in (
                            ('<Ecad>', '<Content><DictionaryStandard units="MILLIMETER">'
                             '<EntryStandard id="C1"><Circle diameter="0.2"/></EntryStandard>'
                             '</DictionaryStandard></Content><Ecad>'),
                            ('<LayerFeature layerRef="F.Cu"><Set>', '<LayerFeature layerRef="F.Cu">'
                             '<Set><Location x="2" y="2"/><StandardPrimitiveRef id="C1"/></Set><Set>'),
                        ):
                            expect(xml.count(anchor) == 1, f'{case}:{path}',
                                   f'exactly one XML anchor {anchor!r}', xml.count(anchor))
                            xml = xml.replace(anchor, replacement, 1)
                        path.write_text(xml, encoding='utf-8')
                    result = a.run(*(['--accept-b-tier'] if rc in (0, 7) else []))
                    summary_path = a.report
                else:
                    board = 'unsupported.xml' if case in ('ecad_unconfirmed', 'ecad_accepted') else 'board_2layer.xml'
                    summary_path = root / 'defects.json'
                    mapping = root / 'missing.json' if rc == 9 else SOURCE / 'tests/integration/import_cli/material_map.json'
                    args = [str(BUILD / 'kernel/chipcae-import'),
                            str(SOURCE / 'tests/integration/ecad/data' / board),
                            '-o', str(root / 'out.stack.json'), '--material-map', str(mapping),
                            '--report', str(summary_path)]
                    if case == 'ecad_accepted':
                        args.append('--accept-b-tier')
                    result = subprocess.run(args, cwd=root, capture_output=True, text=True, timeout=25)
                check(result, rc)
                summary = check_summary(summary_path, assembly, rc)
                report_path = root / 'import_report.json'
                if rc not in (9, 14) or report_path.exists():
                    report = check_import_report(report_path, assembly)
                    if case in ('ecad_unconfirmed', 'assembly_unconfirmed'):
                        expect(bool(b_ids(report)), f'{case}:{report_path}.defects', 'nonempty B IDs', b_ids(report))
                    if case == 'assembly_unconfirmed':
                        expect(set(b_ids(report)) == set(b_ids(summary)), f'{case}:{summary_path}.defects',
                               f'B IDs matching {b_ids(report)!r}', b_ids(summary))
                    if case == 'ecad_accepted':
                        expect(report['b_tier_accepted'] == 'all', f'{case}:{report_path}.b_tier_accepted',
                               'all', report['b_tier_accepted'])
                    if rc == 7:
                        expect(any(d['tier'] == 'C' and d['defect_id'].startswith('board/')
                                   for d in report['defects']), f'{case}:{report_path}.defects',
                               'at least one C defect with board/ ID', report['defects'])


if __name__ == '__main__':
    unittest.main(argv=[__file__], verbosity=2)
