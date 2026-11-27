"""Synthetic thermal-bow acceptance for the opt-in Q2 pipeline sidecar."""
import copy
import importlib.util
import json
import math
import os
from pathlib import Path
import re
import resource
import shlex
import struct
import subprocess
import sys
import tempfile
import time
import zlib

# thermal_bow exits 77 without these packages: intercept that before importing it.
for dependency in ('numpy', 'h5py'):
    if importlib.util.find_spec(dependency) is None:
        print(f'FAIL: required dependency {dependency} is missing', flush=True)
        sys.exit(1)
try:
    import h5py
    import numpy as np
except ImportError as error:
    print(f'FAIL: required dependency: {error}', flush=True)
    sys.exit(1)

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_fem_thermal_bow import card, reference_u, rigid_residual
from q2_export_check import (EPS, CheckError, area_centroid, check_values, clip_rectangle,
                             prepare, read_files, require)


def reference(p):
    return reference_u(p, 300, 400, 300, 1e-5, 1e-5)


def logs(path):
    content, offset, result = path.read_bytes(), 0, []
    while offset < len(content):
        assert content[offset:offset+8] == b'CAELOG1\0', 'event magic'
        length, crc = struct.unpack_from('<II', content, offset+8)
        body = content[offset+16:offset+16+length]
        assert len(body) == length and zlib.crc32(body) == crc, 'event framing/CRC'
        _, _, kind = struct.unpack_from('<QqB', body)
        if kind == 20:
            level, rank, size = struct.unpack_from('<iiI', body, 17)
            message = body[29:29+size].decode('utf-8')
            assert len(body[29:29+size]) == size
            result.append((rank, level, message))
        offset += 16+length
    return result


def geometry(cut):
    cells, cuts, nodes = set(), set(), set()
    for i in range(4):
        for j in range(4):
            # Derive retained/cut columns by clipping, independent of exported IDs.
            area, _ = area_centroid(clip_rectangle(i/4, (i+1)/4, j/4, (j+1)/4))
            if cut and area == 0:
                continue
            for k in range(4):
                cells.add((i, j, k))
                if cut and area < 1/16-32*EPS:
                    cuts.add((i, j, k))
                nodes.update((2*i+a, 2*j+b, 2*k+c)
                             for a in range(3) for b in range(3) for c in range(3))
    assert (len(cells), len(cuts), len(nodes)) == ((60, 20, 693) if cut else (64, 0, 729))
    return cells, nodes


def grid_index(p):
    return tuple(int(round(float(v)*s)) for v, s in zip(p, (8, 8, 80)))


def check_geometry(elements, cut):
    expected_cells, expected_nodes = geometry(cut)
    cells, nodes = set(), set()
    extension_nodes = 0
    for e in elements:
        lo = np.array([v[0] for v in e['levels']])
        cell = tuple(int(round(float(v)*s)) for v, s in zip(lo, (4, 4, 40)))
        require(cell not in cells, 'COUNT', 'duplicate background cell')
        cells.add(cell)
        i, j, k = cell
        actual = {grid_index(p) for p in e['p']}
        want = {(2*i+a, 2*j+b, 2*k+c) for a in range(3) for b in range(3) for c in range(3)}
        require(actual == want, 'COUNT', 'incomplete background Q2 nodes')
        nodes.update(actual)
        extension_nodes += sum(p[0]+p[1] > 1.4 for p in e['p'])
    require(cells == expected_cells and nodes == expected_nodes, 'COUNT',
            f'background cells/nodes={len(cells)}/{len(nodes)}')
    if cut:
        require(extension_nodes > 0, 'COUNT', 'missing cut-cell extension nodes')


def vtk_arrays(out):
    manifest = json.loads((out/'manifest.json').read_text())
    with h5py.File(out/manifest['steps'][0]['file']) as stream:
        group = stream['VTKHDF/hex']
        arrays = {key: group[key][:] for key in
                  ('Points', 'PointData/U', 'PointData/T', 'CellData/elem_gid')}
    return manifest, arrays


def compare_vtk(elements, arrays):
    nodes = {}
    for e in elements:
        for p, u, t in zip(e['p'], e['u'], e['t']):
            # Hex output contains background vertices for the uncut cells.
            index = grid_index(p)
            if all(v % 2 == 0 for v in index):
                nodes[index] = (p, u, t)
    umax = max(float(np.max(abs(e['u']))) for e in elements)
    tmax = max(float(np.max(abs(e['t']))) for e in elements)
    matched = 0
    for p, u, t in zip(arrays['Points'], arrays['PointData/U'], arrays['PointData/T']):
        key = grid_index(p)
        require(key in nodes, 'COUNT', 'VTKHDF vertex absent from Q2 background cells')
        q, v, s = nodes[key]
        require(np.max(abs(p-q)) <= 4*EPS*max(float(np.max(abs(p))), float(np.max(abs(q)))),
                'VALUE', 'VTKHDF coordinate fidelity')
        require(np.max(abs(u-v)) <= 256*EPS*umax and abs(t-s) <= 256*EPS*tmax,
                'VALUE', 'VTKHDF field fidelity')
        matched += 1
    require(matched > 0, 'COUNT', 'no VTKHDF vertices checked')


def expect_code(code, documents, strict=True):
    try:
        elements = prepare(documents, strict=strict)
        check_values(elements, reference, rigid_residual, cut=True)
    except CheckError as error:
        assert error.code == code, f'expected {code}, got {error}'
    else:
        raise AssertionError(f'{code}: corrupt sidecar accepted')


def mutations(documents):
    for code, key in [('ORDER', 'points'), ('COUNT', None), ('VALUE', 'U')]:
        bad = copy.deepcopy(documents)
        e = bad[0]['elements'][0]
        if key:
            a, b = (0, 13) if key == 'points' else (1, 2)
            e[key][a], e[key][b] = e[key][b], e[key][a]
        else:
            for field in ('points', 'U', 'T'):
                e[field].pop()
        expect_code(code, bad)
    permuted = copy.deepcopy(documents)
    e = permuted[0]['elements'][0]
    for field in ('points', 'U', 'T'):
        e[field][0], e[field][13] = e[field][13], e[field][0]
    check_values(prepare(permuted, strict=False), reference, rigid_residual, cut=True)
    expect_code('ORDER', permuted)


def run(root, exe, mpiexec, name, ranks=1, cut=False, enabled=True, thermal=False):
    folder = root/name
    (folder/'inputs').mkdir(parents=True)
    outline = dict(box=[0, 0, 1, 1])
    if cut:
        outline['polygon'] = [[0, 0], [1, 0], [1, .4], [.4, 1], [0, 1]]
    stack = dict(id='q2-export-analytic', unit='m', outline=outline,
                 symmetry='none', package_type='FCBGA', layers=[
                     dict(name=n, kind='Explicit', material='plate', thickness=.05,
                          n_elem_min=1, grading=1) for n in ('a', 'b')])
    mesh = dict(type='aligned_cartesian_cut', nx=4, ny=4, levels=1,
                min_elems_per_layer=2, max_ratio=10)
    if cut:
        mesh['cut'] = dict(stabilization='ghost_penalty', quad_degree=6, gamma_g=.1)
    physics = [dict(id='thermal', solver='fem.conduction', mode='steady')]
    if not thermal:
        physics.append(dict(id='mech', solver='fem.solid', temp_from='thermal', T_ref=300))
    case = dict(schema='chipcae.case/1.0', stack=dict(ref='inputs/stack.json'),
                materials=dict(ref='materials.json'), mesh=mesh, physics=physics,
                loads=[dict(type='fixed_temperature', target='face:zmin', T=300),
                       dict(type='fixed_temperature', target='face:zmax', T=400)],
                constraints=dict(rigid='3-2-1'), solver=dict(linear=dict(preconditioner='direct')),
                output=dict(format='vtkhdf'))
    for filename, data in [('inputs/stack.json', stack), ('case.json', case),
                           ('materials.json', dict(schema='chipcae.material_library/1',
                                                   materials=[card('plate', 25e9, .3, 1e-5, 2)])),
                           ('profile.json', dict(schema='chipcae.run_profile/1.0', ranks=ranks,
                                                 backend='cpu', gpus=0))]:
        (folder/filename).write_text(json.dumps(data))
    out = folder/'out'
    command = [exe, '--case', str(folder/'case.json'), '--profile', str(folder/'profile.json'),
               '--out', str(out)]
    if ranks > 1:
        assert mpiexec, 'B-box-np2 requires MPIEXEC_EXECUTABLE'
        command = [mpiexec, '-np', str(ranks), *command]
    env = dict(os.environ, OMP_NUM_THREADS='1', OPENBLAS_NUM_THREADS='1',
               OMPI_ALLOW_RUN_AS_ROOT='1', OMPI_ALLOW_RUN_AS_ROOT_CONFIRM='1')
    env.pop('CHIPCAE_ACCURACY_Q2_EXPORT', None)
    if enabled:
        env['CHIPCAE_ACCURACY_Q2_EXPORT'] = '1'
    print('COMMAND', name, shlex.join(command), flush=True)
    start = time.perf_counter()
    process = subprocess.run(command, cwd=folder, env=env, capture_output=True, text=True, timeout=120)
    elapsed = time.perf_counter()-start
    rss = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss/(1048576 if sys.platform == 'darwin' else 1024)
    print(f'实测 {name}: seconds={elapsed:.6f} peak_RSS_MiB={rss:.6f}', flush=True)
    assert rss <= 15360, f'{name}: RSS={rss} MiB > 15360'
    assert process.returncode == 0, f'{name}: CLI exit={process.returncode}\n{process.stderr}'
    files = sorted(out.glob('accuracy-q2-rank*.json'))
    messages = [m for _, _, m in logs(out/'events.bin') if 'q2_export' in m]
    manifest, arrays = vtk_arrays(out)
    if not enabled:
        assert not list(out.glob('accuracy-q2*')), 'B5: default-off sidecars present'
        assert not messages, 'B5: default-off q2_export log present'
        return manifest, arrays
    assert len(files) == ranks, f'{name}: expected {ranks} Q2 sidecar(s), found {len(files)}'
    assert {f.name for f in files} == {f'accuracy-q2-rank{r}.json' for r in range(ranks)}
    documents = read_files(files)
    elements = prepare(documents)
    check_geometry(elements, cut)
    compare_vtk(elements, arrays)
    check_values(elements, reference, rigid_residual, cut=cut, thermal=thermal)
    # Existing event persistence is rank 0 only. Validate its local statistics.
    assert len(messages) == 1, f'{name}: q2_export log count={len(messages)}'
    match = re.fullmatch(r'q2_export elements=(\d+) nodes=(\d+) bytes=(\d+) seconds=(\S+)', messages[0])
    assert match, messages[0]
    count, nodes, size = map(int, match.groups()[:3])
    seconds = float(match.group(4))
    assert count == len(documents[0]['elements']) and nodes == 27*count
    assert size == files[0].stat().st_size and math.isfinite(seconds) and seconds >= 0
    print(f'实测 {name}: export_bytes={sum(f.stat().st_size for f in files)} '
          f'rank0_export_seconds={seconds:.17g}', flush=True)
    if cut:
        mutations(documents)
    return manifest, arrays


def checker_controls():
    # Analytic controls make checker defects visible even before a sidecar exists.
    cells, _ = geometry(True)
    document = {'elements': []}
    for gid, (i, j, k) in enumerate(sorted(cells)):
        p = np.array([[(2*i+a)/8, (2*j+b)/8, (2*k+c)/80]
                      for c in range(3) for b in range(3) for a in range(3)])
        document['elements'].append(dict(gid=gid, points=p.tolist(), U=reference(p).tolist(),
                                         T=(300+1000*p[:, 2]).tolist()))
    documents = [document]
    check_geometry(prepare(documents), True)
    check_values(prepare(documents), reference, rigid_residual, cut=True)
    mutations(documents)
    print('PASS independent Python checker controls N1/N3/N2/P1', flush=True)


def main():
    checker_controls()
    exe, *mpi = sys.argv[1:]
    failures, results = [], {}
    with tempfile.TemporaryDirectory(prefix='q2-export-pipeline-') as tmp:
        for name, kwargs in [('B-box', {}), ('B-box-np2', dict(ranks=2)),
                             ('B-cut', dict(cut=True)), ('B-box-off', dict(enabled=False)),
                             ('thermal-only', dict(thermal=True))]:
            try:
                results[name] = run(Path(tmp), exe, mpi[0] if mpi else '', name, **kwargs)
            except (AssertionError, subprocess.TimeoutExpired) as error:
                failures.append(f'{name}: {error}')
                print(f'FAIL {name}: {error}', flush=True)
        if 'B-box' in results and 'B-box-off' in results:
            on, off = results['B-box'], results['B-box-off']
            assert on[0]['mesh_sha256'] == off[0]['mesh_sha256'], 'B5: mesh digest'
            for key in on[1]:
                a, b = on[1][key], off[1][key]
                assert a.dtype == b.dtype and a.shape == b.shape and a.tobytes() == b.tobytes(), (
                    f'B5: {key} dtype/shape/bytes changed')
        if failures:
            raise AssertionError('\n'.join(failures))


if __name__ == '__main__':
    main()
