"""Layer alignment, material identity and equivalent copper representations."""
import copy
from pathlib import Path
import tempfile
import json
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
            e = entry(r, 'thickness', key)
            assert e['grade'] == 'hard' and e[missing] is None
            assert e['delta'] is None and e['tol'] is None
            number(e['ipc' if missing == 'odb' else 'odb'], 1e-4)
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
        assert any('3:|extra' in line and 'hard' in line for line in details), md
        assert not any('not compared:' in line for line in details), md
        b = copy.deepcopy(a)
        b['layers'][0], b['layers'][1] = b['layers'][1], b['layers'][0]
        assert entry(diff_stacks(pa, save(pb, b)), 'layers')['grade'] == 'hard'
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
