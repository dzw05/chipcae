"""Column GMG acceptance through CLI events and independent VTKHDF readback."""

import json
import math
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

import h5py
import numpy as np


def events(path):
    content = path.read_bytes()
    result = []
    while content:
        assert content[:8] == b'CAELOG1\x00'
        length, crc = struct.unpack_from('<II', content, 8)
        body = content[16:16+length]
        assert zlib.crc32(body) == crc
        seq, wall, kind = struct.unpack_from('<QqB', body)
        result.append((kind, body[17:]))
        content = content[16+length:]
    return result


def run(exe, mpiexec, source, root, name, nx, ny, levels, preconditioner, ranks=1):
    # Every CLI invocation owns its case, stack, material, profile and output paths.
    work = root / name
    work.mkdir()
    examples = source / 'schema/examples'
    for part in ('case', 'stack', 'materials'):
        filename = f'fcbga_demo.{part}.json'
        shutil.copyfile(examples / filename, work / filename)
    case_path = work / 'fcbga_demo.case.json'
    case = json.loads(case_path.read_text())
    case['mesh'].update(nx=nx, ny=ny, levels=levels)
    case['solver'] = {'linear': {'preconditioner': preconditioner, 'rtol': 1e-10}}
    case_path.write_text(json.dumps(case))
    profile = work / 'profile.json'
    profile.write_text(json.dumps({'schema': 'chipcae.run_profile/1.0',
                                   'ranks': ranks, 'backend': 'cpu', 'gpus': 0}))
    out = work / 'output'
    command = [exe, '--case', str(case_path), '--profile', str(profile), '--out', str(out)]
    if ranks > 1:
        command = [mpiexec, '-np', str(ranks), *command]
    result = subprocess.run(command, capture_output=True, text=True, timeout=840)
    assert result.returncode == 0, (name, command, result.returncode, result.stdout, result.stderr)
    config = re.search(r'run_config levels=(\d+) n0=(\d+)x(\d+)', result.stderr)
    assert config, f'{name}: stderr is missing run_config: {result.stderr}'
    actual_levels, nx0, ny0 = map(int, config.groups())
    assert actual_levels == levels, (name, config.group(0))
    finest = (nx0 * 2**(actual_levels-1), ny0 * 2**(actual_levels-1))
    print(f'实测 {name}: levels={actual_levels} n0={nx0}x{ny0} '
          f'finest={finest[0]}x{finest[1]}', flush=True)
    manifest = json.loads((out / 'manifest.json').read_text())
    summary = {entry['name']: entry['value'] for entry in manifest['summary']}
    pv = summary['warpage_pv']
    assert np.isfinite(pv) and pv > 0, (name, summary)
    with h5py.File(out / manifest['steps'][0]['file']) as data:
        assert np.sum(data['VTKHDF/cut/NumberOfCells'][:]) == 0, f'{name}: fixture must be uncut'
        displacement = data['VTKHDF/hex/PointData/U'][:]
        points = data['VTKHDF/hex/Points'][:]
    assert displacement.shape == points.shape and displacement.shape[1] == 3
    assert np.all(np.isfinite(displacement)), name
    ev = events(out / 'events.bin')
    assert ev and ev[-1][0] == 19, f'{name}: missing KernelExit'
    logs = [payload for kind, payload in ev if kind == 20]
    return {'name': name, 'levels': actual_levels, 'finest': finest,
            'hash': manifest['mesh']['mesh_sha256'], 'u': displacement,
            'points': points, 'pv': pv, 'logs': logs}


def gmg_stats(result):
    matches = [re.search(rb'U column_gmg L=(\d+) iterations=(\d+) '
                         rb'relative_residual=(\S+) setup_seconds=(\S+) solve_seconds=(\S+)', log)
               for log in result['logs']]
    matches = [match for match in matches if match]
    assert len(matches) == 1, (f"{result['name']}: events are missing the U column_gmg L= line",
                               result['logs'])
    assert not any(b'not yet available' in log for log in result['logs']), result['logs']
    level, iterations, residual, setup, solve = matches[0].groups()
    level, iterations = int(level), int(iterations)
    residual, setup, solve = float(residual), float(setup), float(solve)
    assert level == result['levels'] - 1 and iterations > 0, matches[0].group(0)
    assert all(math.isfinite(value) and value >= 0 for value in (residual, setup, solve))
    print(f"实测 {result['name']}: iterations={iterations} setup_seconds={setup} "
          f'solve_seconds={solve} relative_residual={residual}', flush=True)
    return iterations


def lu_stats(result):
    matches = [re.search(rb'U assembly_seconds=\S+.*? factor_seconds=(\S+) LU_solve_seconds=(\S+)', log)
               for log in result['logs']]
    matches = [match for match in matches if match]
    assert len(matches) == 1, (f"{result['name']}: missing U LU timing line", result['logs'])
    factor, solve = map(float, matches[0].groups())
    assert all(math.isfinite(value) and value >= 0 for value in (factor, solve))
    print(f"实测 {result['name']}: factor_seconds={factor} LU_solve_seconds={solve}", flush=True)


def compare(reference, gmg):
    assert gmg['hash'] == reference['hash'], (reference['name'], gmg['name'], 'mesh hashes differ')
    assert gmg['u'].shape == reference['u'].shape
    np.testing.assert_allclose(gmg['points'], reference['points'], rtol=0, atol=1e-14)
    scale = np.max(np.abs(reference['u']))
    assert scale > 0, 'reference displacement must be nonzero'
    u_error = np.max(np.abs(gmg['u'] - reference['u'])) / scale
    pv_error = abs(gmg['pv'] - reference['pv']) / abs(reference['pv'])
    print(f"实测 {gmg['name']}: relative_U={u_error} relative_PV={pv_error}", flush=True)
    assert u_error <= 1e-8, (gmg['name'], 'relative U', u_error)
    assert pv_error <= 1e-8, (gmg['name'], 'relative PV', pv_error)


def main():
    exe, mpiexec, source = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix='chipcae-column-gmg-') as tmp:
        root = Path(tmp)
        iterations = []
        references = []
        gmgs = []
        for size, levels, label in ((8, 2, 'coarse'), (16, 3, 'fine')):
            direct = run(exe, mpiexec, Path(source), root, label + '-direct',
                         size, size, levels, 'direct')
            gmg = run(exe, mpiexec, Path(source), root, label + '-gmg',
                      size, size, levels, 'column_gmg')
            lu_stats(direct)
            iterations.append(gmg_stats(gmg))
            compare(direct, gmg)
            references.append(direct)
            gmgs.append(gmg)
        assert gmgs[1]['levels'] == gmgs[0]['levels'] + 1, 'fine hierarchy must add one level'
        assert iterations[1] <= math.ceil(1.3 * iterations[0]), ('iteration growth', iterations)
        fallback = run(exe, mpiexec, Path(source), root, 'fine-gmg-np2',
                       16, 16, 3, 'column_gmg', ranks=2)
        expected = b'not yet available (NOT_IMPLEMENTED: column_gmg unsupported: np=2)'
        assert any(expected in log for log in fallback['logs']), fallback['logs']
        assert not any(b'U column_gmg L=' in log for log in fallback['logs']), fallback['logs']
        lu_stats(fallback)
        assert fallback['hash'] == references[1]['hash'], 'np=2 must preserve the mesh'
        pv_error = abs(fallback['pv'] - references[1]['pv']) / abs(references[1]['pv'])
        print(f'实测 fine-gmg-np2: relative_PV={pv_error}', flush=True)
        assert pv_error <= 1e-8, ('np=2 fallback relative PV', pv_error)


if __name__ == '__main__':
    main()
