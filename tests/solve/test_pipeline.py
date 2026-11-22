"""Numerical acceptance through the public CLI and independent VTKHDF readback."""
import copy
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import time
import zlib

import h5py
import numpy as np

exe, mpirun = sys.argv[1:]

def card(name, E, nu, alpha, k=1):
    def table(values):
        return {'temperatures': [300], 'columns': {key: [v] for key, v in values.items()},
                'interpolation': 'constant', 'extrapolation': 'clamp'}
    return {'id': name, 'schema_version': '1', 'unit_system': 'SI', 'model': 'thermo_elastic',
            'density': 2000, 'conductivity': k,
            'elastic': {'symmetry': 'isotropic', 'table': table({'E': E, 'nu': nu})},
            'thermal_expansion': {'symmetry': 'isotropic', 'cte_type': 'tangent',
                'stress_free_temperature': 300, 'table': table({'alpha': alpha})},
            'provenance': {'source_type': 'illustrative', 'confidence': 'illustrative'}}


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

with tempfile.TemporaryDirectory() as tmp:
    root = Path(tmp)
    case = {'schema': 'chipcae.case/1.0', 'stack': {'ref': 'stack.json'},
            'materials': {'ref': 'materials.json'},
            'mesh': {'type': 'aligned_cartesian_cut', 'nx': 8, 'ny': 8, 'levels': 1,
                     'min_elems_per_layer': 1, 'max_ratio': 10},
            'physics': [{'id': 'mech', 'solver': 'fem.solid'}],
            'loads': [{'type': 'uniform_temperature', 'T_ref': 400, 'T_target': 300}],
            'constraints': {'rigid': '3-2-1'}, 'solver': {'linear': {'preconditioner': 'direct'}},
            'output': {'format': 'vtkhdf'}}
    stack = {'id': 'acceptance', 'unit': 'm', 'outline': {'box': [0, 0, 1, 1]},
             'symmetry': 'none', 'package_type': 'FCBGA', 'layers': []}

    def run(name, c, layers, materials, ranks=1, control=None, copper=None):
        stack['layers'] = [{'name': ident, 'kind': 'Explicit', 'material': ident,
                            'thickness': thickness, 'n_elem_min': 1, 'grading': 1}
                           for ident, thickness in layers]
        if copper is not None:
            for layer in stack['layers']:
                layer.update(kind='Patterned', copper={'metal': 'copper', 'fraction': copper})
        (root / 'stack.json').write_text(json.dumps(stack))
        (root / 'materials.json').write_text(json.dumps({'schema': 'chipcae.material_library/1',
                                                        'materials': materials}))
        (root / 'case.json').write_text(json.dumps(c))
        (root / 'profile.json').write_text(json.dumps({'schema': 'chipcae.run_profile/1.0',
                         'ranks': ranks, 'backend': 'cpu', 'gpus': 0}))
        out = root / name
        out.mkdir()
        if control is not None:
            (out / 'control.jsonl').write_text(control)
        command = [exe, '--case', str(root / 'case.json'), '--profile',
                   str(root / 'profile.json'), '--out', str(out)]
        if ranks > 1:
            command = [mpirun, '-np', str(ranks), *command]
        start = time.perf_counter()
        result = subprocess.run(command, capture_output=True, text=True, timeout=180)
        assert result.returncode == 0, result.stderr
        print(name, 'seconds=', time.perf_counter() - start, flush=True)
        ev = events(out / 'events.bin')
        assert ev[-1][0] == 19
        if control is not None:
            return out, ev
        manifest = json.loads((out / 'manifest.json').read_text())
        summary = {s['name']: s['value'] for s in manifest['summary']}
        assert all(np.isfinite(v) for v in summary.values())
        assert {10, 11, 19, 21, 22} <= {kind for kind, _ in ev}
        divisor = 2 ** (c['mesh']['levels'] - 1)
        cells = len(layers) * ((c['mesh']['nx']+divisor-1)//divisor*divisor) * ((c['mesh']['ny']+divisor-1)//divisor*divisor)
        with h5py.File(out / manifest['steps'][0]['file']) as f:
            g = f['VTKHDF/hex']
            points = g['Points'][:]
            T = g['PointData/T'][:] if 'T' in g['PointData'] else None
            U = g['PointData/U'][:] if 'U' in g['PointData'] else None
            gids = g['CellData/elem_gid'][:] if 'elem_gid' in g['CellData'] else None
            assert g['Connectivity'].size == cells * 8
            if gids is not None:
                # Reconstruct global numbering independently from partition-local VTK data.
                axes = [sorted(set(points[:, d])) for d in range(3)]
                def morton(x, y):
                    return sum(((x >> b) & 1) << (2*b) | ((y >> b) & 1) << (2*b+1)
                               for b in range(32))
                columns = sorted(((x, y) for x in range(len(axes[0]))
                                  for y in range(len(axes[1]))), key=lambda xy: morton(*xy))
                ordered = [(axes[0][x], axes[1][y], z) for x, y in columns for z in axes[2]]
                node_ids = {p: i for i, p in enumerate(ordered)}
                global_ids = [node_ids[tuple(p)] for p in points]
                conn = g['Connectivity'][:].reshape(-1, 8)
                canonical = []
                start_node = start_cell = 0
                for npart, ncell in zip(g['NumberOfPoints'][:], g['NumberOfCells'][:]):
                    for e in range(int(ncell)):
                        canonical.append((int(gids[start_cell+e]),
                            [global_ids[start_node+int(v)] for v in conn[start_cell+e]]))
                    start_node += int(npart)
                    start_cell += int(ncell)
                meta = manifest['mesh']
                raw = struct.pack('<6q', meta['nx0'], meta['ny0'], meta['L'], meta['nz'],
                                  len(ordered), len(canonical))
                raw += np.asarray(ordered, dtype='<f8').tobytes()
                raw += np.asarray([row for _, row in sorted(canonical)], dtype='<i8').tobytes()
                assert meta['mesh_sha256'] == hashlib.sha256(raw).hexdigest()

            if gids is not None:
                assert sorted(gids.tolist()) == list(range(cells))
        return manifest, summary, points, T, U, ev

    materials = [card('substrate', 25e9, .3, 15e-6), card('silicon', 130e9, .3, 2.6e-6)]
    layers = [('substrate', .05), ('silicon', .05)]
    m, s, p, t, u, ev = run('bimaterial', case, layers, materials)
    mask = (np.abs(p[:, 2]) < 1e-12) & (np.abs(p[:, 0]-.5) <= .25+1e-12) & (np.abs(p[:, 1]-.5) <= .25+1e-12)
    x, y = p[mask, 0]-.5, p[mask, 1]-.5
    fit = np.linalg.lstsq(np.array([x*0+1, x, y, x*x, x*y, y*y]).T, u[mask, 2], rcond=None)[0]
    curvature = -(fit[3]+fit[5])
    ratio = 25/130
    analytic = 6*(2.6e-6-15e-6)*(-100)*4/(.1*(12+(1+ratio)*(1+1/ratio)))
    error = abs(curvature-analytic)/analytic
    print('Timoshenko relative error=', error, flush=True)
    assert error <= .01
    assert np.max(abs(t-300)) < 1e-12
    fallback = copy.deepcopy(case)
    fallback['solver']['linear']['preconditioner'] = 'column_gmg'
    m2, s2, p2, t2, u2, ev2 = run('bimaterial-mpi-gmg', fallback, layers, materials, 2)
    assert abs(s2['warpage_pv']/s['warpage_pv']-1) <= 1e-8
    assert m2['mesh']['mesh_sha256'] == m['mesh']['mesh_sha256']
    assert any(b'not yet available' in payload for kind, payload in ev2 if kind == 20)

    tri_layers = [('substrate', .05), ('attach', .01), ('silicon', .04)]
    tri_materials = [card('substrate',25e9,.2,15e-6),card('attach',3e9,.35,50e-6),card('silicon',130e9,.28,2.6e-6)]
    _, s, *_ = run('trilayer', case, tri_layers, tri_materials)
    error = abs(s['warpage_pv']/4.269532769e-3-1)
    print('trilayer PV=', s['warpage_pv'], 'relative error=', error, flush=True)
    assert error <= 1e-6

    thermal = copy.deepcopy(case)
    thermal['mesh'].update(nx=2, ny=2)
    thermal['physics'] = [{'id':'thermal','solver':'fem.conduction','mode':'steady'},
                          {'id':'mech','solver':'fem.solid','temp_from':'thermal','T_ref':300}]
    thermal['loads'] = [{'type':'fixed_temperature','target':'face:zmin','T':300},
                        {'type':'fixed_temperature','target':'face:zmax','T':400}]
    # Two conductivities: constant flux, piecewise linear exact Q2 solution.
    materials = [card('a',25e9,.3,1e-5,2),card('b',25e9,.3,1e-5,4)]
    layers = [('a',.05),('b',.05)]
    _, _, p, T, *_ = run('thermal-fixed',thermal,layers,materials)
    resistance = .05/2+.05/4
    r = np.minimum(p[:,2],.05)/2+np.maximum(p[:,2]-.05,0)/4
    error = np.max(abs(T-(300+100*r/resistance)))
    print('fixed thermal max error K=',error,flush=True)
    assert error <= 1e-10
    thermal['loads'][1] = {'type':'convection','target':'face:zmax','h':20,'T_inf':400}
    _, _, p, T, *_ = run('thermal-convection',thermal,layers,materials)
    flux = 100/(resistance+1/20)
    r = np.minimum(p[:,2],.05)/2+np.maximum(p[:,2]-.05,0)/4
    error = np.max(abs(T-(300+flux*r)))
    print('convection thermal max error K=',error,flush=True)
    assert error <= 1e-8
    thermal['loads'] = [{'type':'fixed_temperature','target':'face:zmin','T':300},
                        {'type':'fixed_temperature','target':'face:zmax','T':300},
                        {'type':'heat_source','target':'layer:a','q_vol':1000},
                        {'type':'heat_source','target':'layer:b','power':50}]
    same_materials = [card('a',25e9,.3,1e-5,2),card('b',25e9,.3,1e-5,2)]
    _, _, p, T, *_ = run('thermal-source',thermal,layers,same_materials)
    error = np.max(abs(T-(300+1000/(2*2)*p[:,2]*(.1-p[:,2]))))
    print('heat source max error K=',error,flush=True)
    assert error <= 1e-10
    # One element through thickness: the Q1 visualization has only boundary
    # vertices (300 K), while the full Q2 solution peaks at its interior node.
    projected = copy.deepcopy(thermal)
    projected['loads'] = [thermal['loads'][0], thermal['loads'][1],
                          {'type':'heat_source','target':'layer:a','power':100}]
    _, summary, p, T, *_ = run('q1-output', projected, [('a', .1)], same_materials)
    assert np.max(abs(T-300)) < 1e-10
    assert abs(summary['T_max']-300.625) < 1e-10
    # STOP is read after T, before U/output. Ignore malformed/duplicate/partial lines.
    control = 'bad JSON\n' + json.dumps({'cmd_seq':'1','action':'STOP'}) + '\n' + json.dumps({'cmd_seq':'2','action':'STOP'})
    out, ev = run('stop',thermal,layers,materials,2,control)
    early = json.loads((out/'manifest.json').read_text())
    assert early['steps'] == [] and len(early['mesh']['mesh_sha256']) == 64
    assert sum(kind==11 for kind,_ in ev)==1
    assert sum(kind==17 for kind,_ in ev)==1
    assert b'user_stop' in ev[-1][1]
    ack = next(payload for kind,payload in ev if kind == 17)
    assert struct.unpack_from('<Q', ack, 16)[0] == 1
    control = json.dumps({'cmdSeq':'1','action':'PAUSE'})+'\n'+json.dumps({'cmd_seq':'1','action':'STOP'})+'\n'+json.dumps({'cmd_seq':'2','action':'STOP'})
    out, ev = run('control-partial-duplicate',thermal,layers,materials,control=control)
    assert (out/'manifest.json').exists()
    assert sum(kind==11 for kind,_ in ev)==2
    assert sum(kind==17 for kind,_ in ev)==1
    assert b'end_reached' in ev[-1][1]
    selected = copy.deepcopy(case)
    selected['mesh'].update(nx=2, ny=2)
    selected['output']['fields'] = ['U']
    m, _, _, T, U, _ = run('selected-fields',selected,layers,materials)
    assert T is None and U is not None
    assert {f['name'] for f in m['fields'] if not f.get('reserved')} == {'U'}
    layer_fixed = copy.deepcopy(thermal)
    layer_fixed['loads'] = [{'type':'fixed_temperature','target':'layer:a','T':350},
                            {'type':'convection','target':'layer:b','h':20,'T_inf':350}]
    layer_fixed['constraints'] = {'fixed':[{'target':'layer:a'}]}
    layer_materials = [card('a',25e9,.3,0,2),card('b',25e9,.3,0,2)]
    _, _, _, T, U, _ = run('layer-targets',layer_fixed,layers,layer_materials)
    assert np.max(abs(T-350)) < 1e-10 and np.max(abs(U)) == 0
    mixture_case = copy.deepcopy(selected)
    mixture_case['output']['fields'] = ['T', 'U', 'elem_gid']
    materials = [card('a',25e9,.3,1e-5,2),card('b',25e9,.3,1e-5,2),card('copper',110e9,.34,17e-6,400)]
    for fraction in (0, .3, 1):
        _, _, p, _, U, _ = run('copper-'+str(fraction),mixture_case,layers,materials,copper=fraction)
        alpha = (fraction*110e9/(1-.34)*17e-6+(1-fraction)*25e9/(1-.3)*1e-5)/(fraction*110e9/(1-.34)+(1-fraction)*25e9/(1-.3))
        nu = fraction*.34+(1-fraction)*.3
        alpha_z = (fraction*110e9*17e-6+(1-fraction)*25e9*1e-5)/(fraction*110e9+(1-fraction)*25e9)
        expected = p*np.array([alpha,alpha,alpha_z])*(-100)
        assert np.max(abs(U-expected)) < 1e-11
    padded = copy.deepcopy(mixture_case)
    padded['mesh'].update(nx=3, ny=5, levels=2)
    _, _, p, _, U, ev = run('padding-3x5', padded, layers, materials, copper=.3)
    assert any(b'constrained_dofs=6' in payload for kind, payload in ev if kind == 20)
    assert np.max(abs(U)) > 0 and np.all(np.isfinite(U))
    fixed = copy.deepcopy(mixture_case)
    fixed['constraints'] = {'fixed':[{'target':'face:xmin'}]}
    materials = [card('a',25e9,.3,0,2),card('b',25e9,.3,0,2)]
    _, _, _, _, U, _ = run('fixed-face',fixed,layers,materials)
    assert np.max(abs(U)) == 0
print('All pipeline acceptance checks passed',flush=True)
