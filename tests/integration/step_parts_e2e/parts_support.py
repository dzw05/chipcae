"""SP7 acceptance fixtures; all CAD and solver output lives in private temp dirs.

CTest starts scripts in the binary test directory. Resolve tools from that
build's CMakeCache, never from an unrelated source tree or PATH installation.
SP7_STRICT_ACCEPTANCE=1 exposes dependency failures for the red TDD run.
"""
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'tests/contract'))
from output_contract import validate_manifest, validate_output
sys.path.insert(0, str(ROOT / 'tools/samples/step_parts'))
from make_parts import (box, write_step, prepare, dump, box_parts,
                        rounded_parts, cylinder, chamfer_lid)


class DependencyMissing(AssertionError):
    pass


def cli(name):
    build = Path(os.environ.get('CHIPCAE_TEST_BUILD_DIR', Path.cwd())).resolve()
    while not (build / 'CMakeCache.txt').exists():
        assert build != build.parent, 'Run from a build tree or set CHIPCAE_TEST_BUILD_DIR'
        build = build.parent
    relative = {'import': 'kernel', 'preview': 'preview', 'solve': 'apps/chipcae-solve'}[name]
    path = build / relative / f'chipcae-{name}'
    assert path.is_file(), f'Build required CLI: {path}'
    return str(path)


def run(name, args, expected=0):
    command = [cli(name), *map(str, args)]
    result = subprocess.run(command, capture_output=True, text=True, timeout=45)
    print('$', ' '.join(command), '\nexit:', result.returncode,
          '\nstdout:', result.stdout, '\nstderr:', result.stderr, flush=True)
    if (name == 'import' and result.returncode == 2 and result.stderr.strip() in
            ('unknown argument: --assembly', 'not implemented yet: --assembly')):
        raise DependencyMissing('SP4 assembly CLI missing: ' + result.stderr.strip())
    assert result.returncode == expected, (command, result.returncode, expected, result.stderr)
    return result


def occt():
    try:
        import OCP
    except ImportError:
        print('SKIP: fixture generator requires OCCT Python bindings (cadquery-ocp)')
        sys.exit(77)


def assembly(parts):
    return {'schema': 'chipcae.assembly/0.1',
            'board': {'path': 'board.xml', 'format': 'ipc2581', 'material_map': 'map.json'},
            'materials': 'materials.json', 'parts': parts}


def part(ident, material, z=.0005):
    return {'id': ident, 'step': ident + '.step', 'material': material,
            'placement': {'frame': 'step', 'origin': [0, 0, z]}}


def imported(directory, model):
    dump(directory / 'assembly.json', model)
    run('import', ['--assembly', directory / 'assembly.json', '-o', directory / 'stack.json',
                   '--report', directory / 'report.json', '--accept-b-tier'])
    assert (directory / 'stack.json').is_file()
    return json.loads((directory / 'stack.json').read_text())


def solve(directory, label, n=28, stack='stack.json', materials=()):
    case = json.loads((ROOT / 'examples/s2_solve/case.json').read_text())
    case['stack']['ref'] = stack
    case['materials']['ref'] = 'materials.json'
    case['mesh'].update(nx=n, ny=round(n * 44 / 56), levels=1, min_elems_per_layer=1, max_ratio=100)
    case['output']['fields'] = ['T', 'U', 'layer_id', 'material_id', 'cell_kind', 'elem_gid']
    dump(directory / 'case.json', case)
    run('preview', [directory / stack, '-o', directory / (label + '.vtkhdf'),
                    '--nx', n, '--ny', round(n * 44 / 56), '--max-ratio', 100])
    out = directory / label
    run('solve', ['--case', directory / 'case.json', '--profile', directory / 'profile.json', '--out', out])
    manifest = json.loads((out / 'manifest.json').read_text())
    validate_manifest(ROOT, manifest)
    validate_output(ROOT, out)
    # Same manifest/summary readback as tests/solve/test_pipeline.py.
    summary = {s['name']: s['value'] for s in manifest['summary']}
    assert math.isfinite(summary['warpage_pv'])
    import h5py
    with h5py.File(out / manifest['steps'][0]['file']) as f:
        raw = f['VTKHDF/hex/FieldData/material_names'][()]
        names = {v.decode() if isinstance(v, bytes) else str(v) for v in raw.flat}
        assert set(materials) <= names, (materials, names)
    print('MEASURED', label, summary, flush=True)
    return summary['warpage_pv'], out / manifest['steps'][0]['file']


def volume(path, material):
    """Read SI volumes under docs/geometry/vtkhdf-volume-reader-0.1.md."""
    import h5py
    import numpy as np
    total = 0.
    with h5py.File(path) as f:
        for block in ('hex', 'cut'):
            g = f['VTKHDF/' + block]
            names = [x.decode() if isinstance(x, bytes) else str(x)
                     for x in g['FieldData/material_names'][()].flat]
            ids = g['CellData/material_id'][:]
            # Writer material IDs index the FieldData table (see results contract).
            selected = ids == names.index(material) + 1
            if block == 'cut':
                total += float(np.sum(g['CellData/volume'][:][selected]))
            else:
                start_p = start_c = 0
                for count_p, count_c in zip(g['NumberOfPoints'][:], g['NumberOfCells'][:]):
                    count_p, count_c = int(count_p), int(count_c)
                    cells = g['Connectivity'][start_c*8:(start_c+count_c)*8].reshape(-1, 8)
                    xyz = g['Points'][start_p:start_p+count_p][cells]
                    values = np.prod(np.max(xyz, axis=1) - np.min(xyz, axis=1), axis=1)
                    total += float(np.sum(values[selected[start_c:start_c+count_c]]))
                    start_p += count_p
                    start_c += count_c
    assert total > 0, (material, total)
    return total


def main(test, dependency=None, needs_occt=True):
    if needs_occt:
        occt()
    try:
        with tempfile.TemporaryDirectory(prefix='sp7-') as tmp:
            test(Path(tmp))
    except DependencyMissing as error:
        if os.environ.get('SP7_STRICT_ACCEPTANCE') == '1':
            raise
        print(f'XFAIL ({dependency or "SP4/SP6-A"}): {error}; CTest maps expected dependency failure to skip 77')
        sys.exit(77)
