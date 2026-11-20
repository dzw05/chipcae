"""Exact CLI thermal bow: catches gradient loads, reference offsets and copper wiring.

The symmetric gradient of reference_u is diag(a*theta, a*theta, b*theta):
its xz/yz shear terms cancel. Thus stress is zero and this quadratic lies
in Q2, independently of the stiffness tensor. Only rigid motion is unknown.
Top-plane removal gives PV = abs(a*g)*L**2/4 on the full Q2 top grid.
VTKHDF contains vertices only; use those for U/T and signed bow, and the
manifest's Q2 summary for PV (do not substitute a vertex sampling mean).
"""
import hashlib
import json
import os
from pathlib import Path
import shlex
import struct
import subprocess
import sys
import tempfile
import time

try:
    import h5py
    import numpy as np
except ImportError as error:
    print(f'未执行: {error}', flush=True)
    sys.exit(77)

L, H = 1.0, 0.1
METHOD = 'Q2 top nodes; least-squares plane removal'


def card(name, E, nu, alpha, k, sft=300, temperatures=(300,),
         interpolation='constant'):
    def table(values):
        return dict(temperatures=list(temperatures),
                    columns={key: [v]*len(temperatures) for key, v in values.items()},
                    interpolation=interpolation, extrapolation='clamp')
    return dict(id=name, schema_version='1', unit_system='SI', model='thermo_elastic',
                density=2000, conductivity=k,
                elastic=dict(symmetry='isotropic', table=table(dict(E=E, nu=nu))),
                thermal_expansion=dict(symmetry='isotropic', cte_type='tangent',
                                       stress_free_temperature=sft,
                                       table=table(dict(alpha=alpha))),
                provenance=dict(source_type='illustrative', confidence='illustrative'))


def reference_u(p, bottom, top, tref, a, b):
    x, y, z = p.T
    g, theta0 = (top-bottom)/H, bottom-tref
    return np.column_stack((a*(theta0+g*z)*x, a*(theta0+g*z)*y,
                            b*(theta0*z+g*z*z/2)-a*g*(x*x+y*y)/2))


def rigid_residual(p, delta):
    # Columns are translation and omega cross x; fit all exported samples,
    # including partition duplicates, before any cross-run deduplication.
    matrix = np.zeros((len(p), 3, 6))
    matrix[:, :, :3] = np.eye(3)
    x, y, z = p.T
    matrix[:, 0, 4], matrix[:, 0, 5] = z, -y
    matrix[:, 1, 3], matrix[:, 1, 5] = -z, x
    matrix[:, 2, 3], matrix[:, 2, 4] = y, -x
    matrix = matrix.reshape(-1, 6)
    fit, _, rank, _ = np.linalg.lstsq(matrix, delta.ravel(), rcond=None)
    assert rank == 6, f'rigid fit rank={rank}'
    return np.max(np.abs(delta.ravel()-matrix@fit))


def mixture(f):
    a = (f*110e9*17e-6/(1-.34)+(1-f)*25e9*1e-5/(1-.3)) / (
        f*110e9/(1-.34)+(1-f)*25e9/(1-.3))
    b = (f*110e9*17e-6 + (1-f)*25e9*1e-5) / (f*110e9 + (1-f)*25e9)
    return a, b


def polygons():
    rings = []
    for x in range(4):
        for y in range(4):
            x0, y0 = x*250000, y*250000
            rings.append([(x0, y0), (x0+75000, y0),
                          (x0+75000, y0+250000), (x0, y0+250000)])
    rings.append(rings[0])  # Union must not count this ring twice.
    name = b'a'
    binary = b'CCPOLY01'+struct.pack('<dII', 1e-6, 1, len(name))+name
    binary += struct.pack('<I', len(rings))
    binary += b''.join(struct.pack('<q', 4*i) for i in range(len(rings)+1))
    binary += b''.join(struct.pack('<qq', *p) for ring in rings for p in ring)
    return binary


def readback(out):
    manifest = json.loads((out/'manifest.json').read_text())
    summary = {entry['name']: entry for entry in manifest['summary']}
    assert all(np.isfinite(entry['value']) for entry in summary.values())
    assert summary['warpage_pv']['method'] == METHOD, summary['warpage_pv']
    with h5py.File(out/manifest['steps'][0]['file']) as stream:
        g = stream['VTKHDF/hex']
        p, t, u = g['Points'][:], g['PointData/T'][:], g['PointData/U'][:]
        gids = g['CellData/elem_gid'][:]
        assert np.sum(g['NumberOfCells'][:]) == 64
        assert g['Connectivity'].size == 512
        assert len(gids) == len(np.unique(gids)) == 64
        fraction = g['CellData/copper_fraction'][:] if 'copper_fraction' in g['CellData'] else None
    assert p.shape == u.shape == (len(t), 3)
    assert all(np.all(np.isfinite(v)) for v in (p, t, u, gids))
    if fraction is not None:
        assert len(fraction) == 64 and np.all(np.isfinite(fraction))
    return dict(p=p, t=t, u=u, pv=summary['warpage_pv']['value'], fraction=fraction)


def aligned(result):
    p, index, inverse = np.unique(result['p'], axis=0, return_index=True, return_inverse=True)
    u, t = result['u'][index], result['t'][index]
    # Check shared MPI vertices before selecting one copy per coordinate.
    assert np.max(abs(result['u']-u[inverse])) <= 1e-12*np.max(abs(result['u']))
    assert np.max(abs(result['t']-t[inverse])) <= 1e-10
    assert p.shape == (125, 3), p.shape
    return p, u


def compare(left, right, tolerance, correction=None, rigid=False):
    p, u = aligned(left)
    q, v = aligned(right)
    assert np.array_equal(p, q), 'cross-run coordinate grids differ'
    delta = u-v
    if correction is not None:
        delta -= correction(p)
    residual = rigid_residual(p, delta) if rigid else np.max(abs(delta))
    limit = tolerance*np.max(abs(v))
    assert residual <= limit, (
        f"{left['name']}/{right['name']}: cross-run U residual={residual:.17g}, "
        f'limit={limit:.17g}')


def signed_bow(result, sign):
    p, u = aligned(result)
    top = np.isclose(p[:, 2], H, rtol=0, atol=1e-13)
    xy, w = p[top, :2], u[top, 2]
    plane = np.column_stack((np.ones(len(xy)), xy))
    fit, _, rank, _ = np.linalg.lstsq(plane, w, rcond=None)
    assert rank == 3
    residual = w-plane@fit
    center = residual[np.all(xy == [L/2, L/2], axis=1)]
    corners = residual[np.all((xy == 0) | (xy == L), axis=1)]
    assert len(center) == 1 and len(corners) == 4
    assert np.all(sign*(center[0]-corners) > 0), (center, corners)


def run(root, exe, mpiexec, name, bottom=300, top=400, tref=300,
        ranks=1, copper=None, sidecars=None, linear=False):
    folder = root/name
    (folder/'inputs').mkdir(parents=True)
    stack = dict(id='thermal-bow', unit='m', outline=dict(box=[0, 0, L, L]),
                 symmetry='none', package_type='FCBGA', layers=[
                     dict(name=n, kind='Explicit', material='plate', thickness=H/2,
                          n_elem_min=1, grading=1) for n in ('a', 'b')])
    if copper is not None:
        for layer in stack['layers']:
            layer.update(kind='Patterned', copper=dict(metal='copper', fraction=copper))
    if sidecars:
        stack['layers'][0]['copper'] = dict(
            metal='copper', polygons=dict(file='trace.polyset', layer='a'))
        stack['external_refs'] = [dict(path=n, sha256=hashlib.sha256(data).hexdigest())
                                  for n, data in sidecars.items()]
        for n, data in sidecars.items():
            (folder/'inputs'/n).write_bytes(data)
    c = dict(schema='chipcae.case/1.0', stack=dict(ref='inputs/stack.json'),
             materials=dict(ref='materials.json'),
             mesh=dict(type='aligned_cartesian_cut', nx=4, ny=4, levels=1,
                       min_elems_per_layer=2, max_ratio=10),
             physics=[dict(id='thermal', solver='fem.conduction', mode='steady'),
                      dict(id='mech', solver='fem.solid', temp_from='thermal', T_ref=tref)],
             loads=[dict(type='fixed_temperature', target='face:zmin', T=bottom),
                    dict(type='fixed_temperature', target='face:zmax', T=top)],
             constraints=dict(rigid='3-2-1'), solver=dict(linear=dict(preconditioner='direct')),
             output=dict(format='vtkhdf'))
    kwargs = dict(sft=tref, temperatures=(300, 400) if linear else (300,),
                  interpolation='linear' if linear else 'constant')
    materials = [card('plate', 25e9, .3, 1e-5, 2, **kwargs),
                 card('copper', 110e9, .34, 17e-6, 400, **kwargs)]
    for filename, data in [('inputs/stack.json', stack), ('case.json', c),
                           ('materials.json', dict(schema='chipcae.material_library/1', materials=materials)),
                           ('profile.json', dict(schema='chipcae.run_profile/1.0', ranks=ranks,
                                                 backend='cpu', gpus=0))]:
        (folder/filename).write_text(json.dumps(data))
    command = [exe, '--case', str(folder/'case.json'), '--profile', str(folder/'profile.json'),
               '--out', str(folder/'out')]
    if ranks > 1:
        command = [mpiexec, '-np', str(ranks), *command]
    print('COMMAND', name, shlex.join(command), flush=True)
    start = time.perf_counter()
    try:
        process = subprocess.run(command, cwd=folder, capture_output=True, text=True, timeout=120,
                                 env=dict(os.environ, OMP_NUM_THREADS='1', OPENBLAS_NUM_THREADS='1',
                                          OMPI_ALLOW_RUN_AS_ROOT='1', OMPI_ALLOW_RUN_AS_ROOT_CONFIRM='1'))
    except subprocess.TimeoutExpired as error:
        raise AssertionError(f'{name}: CLI timeout: {error.stderr}') from error
    assert process.returncode == 0, (
        f'{name}: CLI exit {process.returncode}\n'+ '\n'.join(process.stderr.splitlines()[-20:]))
    result = readback(folder/'out')
    result['name'] = name
    p, t, u, pv = (result[key] for key in ('p', 't', 'u', 'pv'))
    a, b = mixture(copper) if copper is not None else (1e-5, 1e-5)
    exact = reference_u(p, bottom, top, tref, a, b)
    residual = rigid_residual(p, u-exact)
    pv_ref = abs(a*(top-bottom)/H)*L*L/4
    error = abs(pv/pv_ref-1) if pv_ref else abs(pv)
    temp_error = np.max(abs(t-(bottom+(top-bottom)/H*p[:, 2])))
    print(f'实测 {name}: PV={pv:.17g} PV_ref={pv_ref:.17g} relative_error={error:.9g} '
          f'U_residual={residual:.9g} T_error={temp_error:.9g} '
          f'elapsed={time.perf_counter()-start:.3f}s', flush=True)
    assert temp_error <= 1e-10, f'{name}: T error={temp_error}'
    if pv_ref:
        assert residual <= 1e-9*np.max(abs(exact)), f'{name}: U residual={residual}'
        assert error <= 1e-9, f'{name}: PV relative error={error}'
    else:
        assert pv <= 1e-14*L, f'{name}: uniform PV={pv}'
        assert np.max(abs(u)) <= 1e-14*L, f'{name}: uniform U={np.max(abs(u))}'
    aligned(result)
    if sidecars:
        assert result['fraction'] is not None, f'{name}: missing copper_fraction'
        assert np.max(abs(result['fraction']-.3)) <= 1e-12, result['fraction']
    return result


def main():
    exe, mpiexec = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix='fem-thermal-bow-') as tmp:
        root = Path(tmp)
        def case(name, **kwargs):
            return run(root, exe, mpiexec, name, **kwargs)
        a = case('A')
        b = case('B', tref=350, linear=True)
        assert abs(b['pv']/a['pv']-1) <= 1e-9
        compare(b, a, 1e-9, correction=lambda p: -50*p*np.array([1e-5]*3), rigid=True)
        c = {f: case(f'C-{f}', copper=f) for f in (0, .3, 1)}
        compare(c[0], a, 1e-12)
        d = case('D', copper=.3, sidecars={'trace.polyset': polygons()})
        compare(d, c[.3], 1e-12)
        assert abs(d['pv']/c[.3]['pv']-1) <= 1e-12
        for name, serial, copper in [('A', a, None), ('C-0.3', c[.3], .3)]:
            parallel = case(name+'-np2', ranks=2, copper=copper)
            assert abs(parallel['pv']/serial['pv']-1) <= 1e-9
        reverse = case('Reverse', bottom=400, top=300, tref=400, linear=True)
        assert abs(reverse['pv']/a['pv']-1) <= 1e-9
        signed_bow(a, 1)
        signed_bow(reverse, -1)
        case('Uniform', top=300)


if __name__ == '__main__':
    main()
