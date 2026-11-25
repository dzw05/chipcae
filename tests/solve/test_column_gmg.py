"""Column GMG acceptance through CLI events and independent VTKHDF readback."""

import argparse
import json
import math
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
import time
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
    # A forbidden np=1 fallback is already a failure. Observe it before the old LU path
    # consumes demo-scale memory; a working GMG path still must finish and pass readback.
    with subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True) as process:
        deadline = time.monotonic() + 840
        try:
            while True:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise subprocess.TimeoutExpired(command, 840)
                try:
                    stdout, stderr = process.communicate(timeout=min(.25, remaining))
                    break
                except subprocess.TimeoutExpired as poll:
                    if preconditioner == 'column_gmg' and ranks == 1:
                        try:
                            live = events(out / 'events.bin')
                        except (FileNotFoundError, AssertionError, struct.error):
                            # An event may be only partially written while the child runs.
                            continue
                        fallback = [payload for kind, payload in live
                                    if kind == 20 and b'not yet available' in payload]
                        if fallback:
                            raise AssertionError((
                                f'{name}: events are missing the U column_gmg L= line; '
                                'np=1 selected the forbidden LU fallback', fallback, poll.stderr
                            )) from None
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.communicate(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.communicate()
        result = subprocess.CompletedProcess(command, process.returncode, stdout, stderr)
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
    # LogLine carries binary code/error fields after its text, so bound numeric tokens.
    matches = [re.search(rb'U column_gmg L=(\d+) iterations=(\d+) '
                         rb'relative_residual=([-+0-9.eE]+) setup_seconds=([-+0-9.eE]+) '
                         rb'solve_seconds=([-+0-9.eE]+)', log)
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
    matches = [re.search(rb'U assembly_seconds=[-+0-9.eE]+.*? factor_seconds=([-+0-9.eE]+) '
                         rb'LU_solve_seconds=([-+0-9.eE]+)', log)
               for log in result['logs'] if struct.unpack_from('<i', log, 4)[0] == 0]
    matches = [match for match in matches if match]
    # Rank 0 owns the solve timing summary; other ranks' relayed LogLines are ignored here.
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
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('exe')
    parser.add_argument('mpiexec')
    parser.add_argument('source', type=Path)
    parser.add_argument('--coverage', choices=('reference', 'scaling'), default='reference')
    args = parser.parse_args()
    exe, mpiexec, source = args.exe, args.mpiexec, args.source
    # Feature-aligned subdivision makes the original 8/16 requests 34x34/68x68.
    # Keep those GMG grids, but bound LU (including the replicated np=2 fallback)
    # with 2/4 requests. Both pairs retain two/three levels and the full FCBGA stack.
    sizes = (8, 16) if args.coverage == 'scaling' else (2, 4)
    with tempfile.TemporaryDirectory(prefix='chipcae-column-gmg-') as tmp:
        root = Path(tmp)
        iterations = []
        references = []
        gmgs = []
        for size, levels, label in zip(sizes, (2, 3), ('coarse', 'fine')):
            label = args.coverage + '-' + label
            gmg = run(exe, mpiexec, source, root, label + '-gmg',
                      size, size, levels, 'column_gmg')
            iterations.append(gmg_stats(gmg))
            gmgs.append(gmg)
            if args.coverage == 'reference':
                direct = run(exe, mpiexec, source, root, label + '-direct',
                             size, size, levels, 'direct')
                lu_stats(direct)
                compare(direct, gmg)
                references.append(direct)
        assert gmgs[1]['levels'] == gmgs[0]['levels'] + 1, 'fine hierarchy must add one level'
        assert gmgs[1]['finest'] == tuple(2 * n for n in gmgs[0]['finest']), \
            'fine grid must double both axes'
        assert iterations[1] <= math.ceil(1.3 * iterations[0]), ('iteration growth', iterations)
        if args.coverage == 'scaling':
            return
        fallback = run(exe, mpiexec, source, root, 'reference-fine-gmg-np2',
                       sizes[1], sizes[1], 3, 'column_gmg', ranks=2)
        expected = b'not yet available (NOT_IMPLEMENTED: column_gmg unsupported: np=2)'
        assert any(expected in log for log in fallback['logs']), fallback['logs']
        assert not any(b'U column_gmg L=' in log for log in fallback['logs']), fallback['logs']
        lu_stats(fallback)
        assert fallback['hash'] == references[1]['hash'], 'np=2 must preserve the mesh'
        pv_error = abs(fallback['pv'] - references[1]['pv']) / abs(references[1]['pv'])
        print(f"实测 {fallback['name']}: relative_PV={pv_error}", flush=True)
        assert pv_error <= 1e-8, ('np=2 fallback relative PV', pv_error)


if __name__ == '__main__':
    main()
