"""Layer alignment, material identity and equivalent copper representations."""
import copy
from pathlib import Path
import tempfile
import json
import re
import subprocess
import sys
from test_identical import AREA, ROOT, board, diff_stacks, entry, number, polyset, reports, save


def main():
    with tempfile.TemporaryDirectory() as tmp:
        directory = Path(tmp)
        a = board()
        pa = save(directory / 'a.json', a)
        b = copy.deepcopy(a)
        b['layers'].append(dict(name='extra', thickness=1e-4, material='mat.fr4'))
        pb = save(directory / 'b.json', b)
        for left, right, key, missing in ((pa, pb, '3:|extra', 'odb'),
                                          (pb, pa, '3:extra|', 'ipc')):
            r = diff_stacks(left, right)
            assert entry(r, 'layers')['grade'] == 'hard'
            assert 'layer count differs' in entry(r, 'layers')['tol_derivation']
            assert entry(r, 'layers')['delta'] == 1
            e = entry(r, 'thickness', key)
            assert e['grade'] == 'hard' and e[missing] is None
            assert e['delta'] is None and e['tol'] is None
            number(e['ipc' if missing == 'odb' else 'odb'], 1e-4)
        # Count differences remain hard even when neither stack has copper.
        dielectric = copy.deepcopy(a)
        for layer in dielectric['layers']:
            layer.pop('copper', None)
        pc = save(directory / 'dielectric.json', dielectric)
        dielectric['layers'].append(dict(name='extra', thickness=1e-4, material='mat.fr4'))
        pd = save(directory / 'dielectric-extra.json', dielectric)
        for left, right in ((pc, pd), (pd, pc)):
            e = entry(diff_stacks(left, right), 'layers')
            assert e['grade'] == 'hard' and e['delta'] == 1
        # A completed comparison exits zero even with hard differences.
        out = directory / 'cli'
        proc = subprocess.run([sys.executable, str(ROOT / 'tools/vv/ecad_diff.py'),
                               '--odb', str(pa), '--ipc', str(pb), '-o', str(out)],
                              capture_output=True, text=True)
        assert proc.returncode == 0, proc.stderr
        result = json.loads((out / 'diff.json').read_text(encoding='utf-8'))
        assert result['summary']['hard'] > 0
        md = reports(result, out)
        details = md.splitlines()
        assert any(r'3:\|extra' in line and 'hard' in line for line in details), md
        assert not any('not compared:' in line for line in details), md
        b = copy.deepcopy(a)
        b['layers'][0], b['layers'][1] = b['layers'][1], b['layers'][0]
        save(pb, b)
        for left, right in ((pa, pb), (pb, pa)):
            e = entry(diff_stacks(left, right), 'layers')
            assert e['grade'] == 'hard' and e['delta'] == 2
            assert 'copper/dielectric sequence differs' in e['tol_derivation']
        # Every aligned layer gets a copper row, including zero-area mismatches.
        for left_copper, right_copper, grade in ((False, False, 'info'),
                                                 (True, False, 'info'),
                                                 (False, True, 'info'),
                                                 (True, True, 'info')):
            left, right = board(), board()
            for stack, present in ((left, left_copper), (right, right_copper)):
                if present:
                    stack['layers'][0]['copper']['fraction'] = 0
                else:
                    del stack['layers'][0]['copper']
            r = diff_stacks(save(directory / 'left.json', left),
                            save(directory / 'right.json', right))
            e = entry(r, 'copper_area', '0:F.Cu|F.Cu')
            assert e['grade'] == grade and e['tol'] == 0
            assert e['odb'] == e['ipc'] == e['delta'] == 0
            assert 'P_layer = 0' in e['tol_derivation']
            layers = entry(r, 'layers')
            assert layers['grade'] == ('info' if left_copper == right_copper else 'hard')
        # Pipes in keys and nested material values remain within Markdown cells.
        b = copy.deepcopy(a)
        b['layers'][0]['material'] = 'mat|FR4'
        md = reports(diff_stacks(pa, save(pb, b)), out)
        assert r'0:F.Cu\|F.Cu' in md and r'mat\|FR4' in md
        for row in md.splitlines():
            if row.startswith('|') and 'hard' in row and 'mat' in row:
                assert len(re.split(r'(?<!\\)\|', row)) == 9, row
        for field in ('material', 'metal'):
            b = copy.deepcopy(a)
            if field == 'metal':
                b['layers'][0]['copper']['metal'] = 'mat.Cu'
            else:
                b['layers'][0]['material'] = 'mat.FR4'
            assert entry(diff_stacks(pa, save(pb, b)), 'material',
                         '0:F.Cu|F.Cu')['grade'] == 'hard'
        b = copy.deepcopy(a)
        polyset(directory, b)
        e = entry(diff_stacks(pa, save(pb, b)), 'copper_area', '0:F.Cu|F.Cu')
        assert e['grade'] == 'info'
        number(e['odb'], AREA)
        number(e['ipc'], AREA)
        number(e['delta'], 0)


if __name__ == '__main__':
    main()
