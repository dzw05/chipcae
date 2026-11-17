"""Exercise actual importer products when the ECAD target is available."""
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    executable = os.environ.get('CHIPCAE_IMPORT_EXE')
    if not executable:
        return 77
    from test_identical import ROOT, diff_stacks, entry, reports, save
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
        assert r['summary']['hard'] == 0 and r['summary']['discuss'] == 0
        print('实测\n' + reports(r, directory / 'same'))
        r = diff_stacks(paths[0], paths[2])
        assert entry(r, 'layers')['grade'] == 'hard'
        print('实测\n' + reports(r, directory / 'different'))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
