"""Synthetic fixtures and public report acceptance; no production helpers used."""
import hashlib
import json
import math
from pathlib import Path
import re
import struct
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))
from tools.vv.ecad_diff import EcadDiffError, diff_stacks, write_report

DELTA = 1e-9
AREA = 0.01 * 0.005
PERIMETER = 0.03


def number(actual, expected):
    assert math.isclose(actual, expected, rel_tol=1e-12,
                        abs_tol=1e-18 if expected == 0 else 0), (actual, expected)


def board():
    layers = []
    for name, thickness in [('F.Cu', 35e-6), ('core', 1e-3), ('B.Cu', 35e-6)]:
        layer = dict(name=name, thickness=thickness, material='mat.fr4',
                     kind='Homogenized', n_elem_min=1, grading=1)
        if name != 'core':
            layer['copper'] = dict(metal='mat.cu', fraction=1.0)
        layers.append(layer)
    return dict(id='synthetic', unit='m', outline={'box': [0, 0, 0.01, 0.005]},
                layers=layers, symmetry='none', package_type='PCB')


def save(path, value):
    path.write_text(json.dumps(value), encoding='utf-8')
    return path


def polyset(directory, stack, width=10000000, trailing=b''):
    name = b'F.Cu'
    points = [(0, 0), (width, 0), (width, 5000000), (0, 5000000)]
    data = (b'CCPOLY01' + struct.pack('<dII', 1e-9, 1, len(name)) + name
            + struct.pack('<Iqq', 1, 0, 4)
            + b''.join(struct.pack('<qq', *p) for p in points) + trailing)
    path = directory / 'copper.polyset'
    path.write_bytes(data)
    stack['external_refs'] = [{'path': path.name, 'sha256': hashlib.sha256(data).hexdigest()}]
    stack['layers'][0]['copper'] = {'metal': 'mat.cu',
                                  'polygons': {'file': path.name, 'layer': 'F.Cu'}}
    return path


def entry(result, item, key=None):
    found = [e for e in result['entries'] if e['item'] == item
             and (key is None or e['key'] == key)]
    assert len(found) == 1, found
    return found[0]


def tolerance(e, delta, perimeter=None):
    number(e['tol'], delta if perimeter is None else delta * perimeter)
    values = [float(s) for s in re.findall(r'(?<![\w.])[-+]?\d+(?:\.\d*)?(?:[eE][-+]?\d+)?',
                                         e['tol_derivation'])]
    assert any(math.isclose(x, delta, rel_tol=1e-12) for x in values), e
    if perimeter is not None:
        assert 'P' in e['tol_derivation'], e
        assert any(math.isclose(x, perimeter, rel_tol=1e-12) for x in values), e


def reports(result, out):
    write_report(result, out)
    first = [(out / name).read_bytes() for name in ('diff.json', 'diff.md')]
    write_report(result, out)
    assert first == [(out / name).read_bytes() for name in ('diff.json', 'diff.md')]
    assert json.loads(first[0]) == result
    assert first[0].decode().rstrip('\n') == json.dumps(result, indent=2, ensure_ascii=False)
    return first[1].decode()


def main():
    with tempfile.TemporaryDirectory() as tmp:
        directory = Path(tmp)
        path = save(directory / 'stack.json', board())
        result = diff_stacks(path, path)
        assert list(result) == ['schema', 'delta_m', 'inputs', 'entries', 'summary']
        assert result['schema'] == 'chipcae.vv.ecad_diff/0.1'
        assert result['inputs'] == {'odb': str(path), 'ipc': str(path)}
        number(result['delta_m'], DELTA)
        expected = [('layers', 'stack')]
        for i, name in enumerate(('F.Cu', 'core', 'B.Cu')):
            expected.extend((item, f'{i}:{name}|{name}') for item in
                            (('thickness', 'material') if i == 1 else
                             ('thickness', 'material', 'copper_area')))
        expected.extend((item, key) for item, key in
                        [('total_thickness', 'stack'), ('outline', 'outline'),
                         ('vias', 'stack'), ('components', 'stack')])
        assert [(e['item'], e['key']) for e in result['entries']] == expected
        assert result['summary'] == {'hard': 0, 'discuss': 0, 'info': len(expected)}
        for e in result['entries']:
            assert list(e) == ['item', 'key', 'odb', 'ipc', 'delta', 'tol',
                               'tol_derivation', 'grade', 'source_loc']
            assert e['delta'] == 0, e
            assert e['source_loc'] is None and e['grade'] == 'info'
        for item in ('vias', 'components'):
            assert entry(result, item)['tol_derivation'] == 'not compared: not present in stack/0.2'
        for e in result['entries']:
            if e['item'] in ('thickness', 'total_thickness'):
                tolerance(e, DELTA)
            if e['item'] in ('outline', 'copper_area'):
                tolerance(e, DELTA, PERIMETER)
        md = reports(result, directory / 'reports')
        assert str(path) in md.splitlines()[0]
        rows = [[cell.strip() for cell in line.strip().strip('|').split('|')]
                for line in md.splitlines() if '|' in line]
        assert any(row[1:] == ['0', '0', str(len(expected))] for row in rows), md
        assert '无' in md
        for item in dict(expected):
            assert any(row[0] == item for row in rows), item
        assert diff_stacks(path, path) == result


if __name__ == '__main__':
    main()
