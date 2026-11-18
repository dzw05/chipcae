"""Exercise actual importer products when the ECAD target is available."""
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    executable = os.environ.get('CHIPCAE_IMPORT_EXE')
    if not executable:
        return 77
    from test_identical import ROOT, DELTA, diff_stacks, entry, number, reports, save
    with tempfile.TemporaryDirectory() as tmp:
        directory = Path(tmp)
        mapping = save(directory / 'material_map.json', {'FR4': 'mat.fr4', 'copper': 'mat.cu'})
        paths = []
        for name, fixture in (('a', 'board_2layer.xml'), ('b', 'board_2layer.xml'),
                              ('c', 'board_4layer.xml')):
            out = directory / name
            out.mkdir()
            path = out / 'stack.json'
            proc = subprocess.run([executable, str(ROOT / 'tests/integration/ecad/data' / fixture),
                                   '-o', str(path), '--polyset', str(out / 'stack.polyset'),
                                   '--report', str(out / 'defects.json'),
                                   '--material-map', str(mapping)], capture_output=True, text=True)
            assert proc.returncode == 0, (proc.stdout, proc.stderr)
            paths.append(path)
        r = diff_stacks(paths[0], paths[1])
        assert list(r) == ['schema', 'delta_m', 'inputs', 'entries', 'summary']
        assert r['schema'] == 'chipcae.vv.ecad_diff/0.1'
        assert r['delta_m'] == DELTA
        assert r['inputs'] == {'odb': str(paths[0]), 'ipc': str(paths[1])}
        expected = [('layers', 'stack')]
        for i, name in enumerate(('F.Cu', 'Core', 'B.Cu')):
            expected.extend((item, f'{i}:{name}|{name}') for item in
                            ('thickness', 'material', 'copper_area'))
        expected.extend([('total_thickness', 'stack'), ('outline', 'outline'),
                         ('vias', 'stack'), ('components', 'stack')])
        assert [(e['item'], e['key']) for e in r['entries']] == expected
        assert r['summary'] == {'hard': 0, 'discuss': 0, 'info': 14}
        assert entry(r, 'layers')['odb'] == ['F.Cu', 'Core', 'B.Cu']
        for e in r['entries']:
            assert list(e) == ['item', 'key', 'odb', 'ipc', 'delta', 'tol',
                               'tol_derivation', 'grade', 'source_loc']
            assert e['grade'] == 'info' and e['source_loc'] is None
            assert e['delta'] == 0 and e['odb'] == e['ipc']
        for i, (name, thickness, area) in enumerate((
                ('F.Cu', 35e-6, 6e-6), ('Core', 1.6e-3, 0), ('B.Cu', 35e-6, 12e-6))):
            key = f'{i}:{name}|{name}'
            number(entry(r, 'thickness', key)['odb'], thickness)
            assert entry(r, 'material', key)['odb'] == {
                'material': 'mat.fr4' if name == 'Core' else 'mat.cu',
                'metal': None if name == 'Core' else 'mat.cu'}
            number(entry(r, 'copper_area', key)['odb'], area)
        number(entry(r, 'total_thickness')['odb'], 1.67e-3)
        outline = entry(r, 'outline')['odb']
        number(outline['area'], 50e-6)
        number(outline['perimeter'], 0.03)
        assert outline['box'] == [0, 0, 0.01, 0.005]
        assert outline['vertices'] == 4
        print('实测\n' + reports(r, directory / 'same'))
        r = diff_stacks(paths[0], paths[2])
        assert entry(r, 'layers')['grade'] == 'hard'
        expected = [('layers', 'stack')]
        for i, (left, right) in enumerate((('F.Cu', 'F.Cu'), ('Core', 'In1.Cu'),
                                            ('B.Cu', 'Core'))):
            expected.extend((item, f'{i}:{left}|{right}') for item in
                            ('thickness', 'material', 'copper_area'))
        expected.extend([('thickness', '3:|In2.Cu'), ('thickness', '4:|B.Cu'),
                         ('total_thickness', 'stack'), ('outline', 'outline'),
                         ('vias', 'stack'), ('components', 'stack')])
        assert [(e['item'], e['key']) for e in r['entries']] == expected
        assert r['summary'] == {'hard': 11, 'discuss': 0, 'info': 5}
        assert entry(r, 'outline')['grade'] == 'hard'
        number(entry(r, 'outline')['ipc']['area'], 25e-6)
        number(entry(r, 'total_thickness')['ipc'], 306e-6)
        print('实测\n' + reports(r, directory / 'different'))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
