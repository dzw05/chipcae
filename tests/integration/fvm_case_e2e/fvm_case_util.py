"""Synthetic fixtures, CLI execution, partition-aware readback and strict CAELOG1 decoding."""

from copy import deepcopy
import hashlib
import json
import math
import os
from pathlib import Path
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import zlib

THERMAL_ONLY = 'not solved: fvm thermal-only stage'
ENERGY_PREFIX = 'FVM_ENERGY '
REQUIRE_COPPER_FIELDS = False


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, allow_nan=False) + '\n', encoding='utf-8')


def read_json(path):
    return json.loads(path.read_text(encoding='utf-8'))


def material(name, k):
    def table(columns):
        return dict(temperatures=[300.0], columns={key: [v] for key, v in columns.items()},
                    interpolation='constant', extrapolation='clamp')
    return dict(id=name, schema_version='1', unit_system='SI', model='thermo_elastic',
                density=2000, conductivity=k,
                elastic=dict(symmetry='isotropic', table=table(dict(E=25e9, nu=.3))),
                thermal_expansion=dict(symmetry='isotropic', cte_type='tangent',
                                       stress_free_temperature=300, table=table(dict(alpha=1e-5))),
                provenance=dict(source_type='illustrative', confidence='illustrative'))


def thermal_case(family='fvm'):
    return dict(
        schema='chipcae.case/1.0', stack=dict(ref='stack.json'),
        materials=dict(ref='materials.json'),
        mesh=dict(type='aligned_cartesian_cut', nx=4, ny=4, levels=0,
                  min_elems_per_layer=1, max_ratio=100,
                  cut=dict(stabilization='ghost_penalty', quad_degree=6, gamma_g=.1)),
        physics=[dict(id='thermal', solver=family + '.conduction', mode='steady'),
                 dict(id='mech', solver=family + '.solid', temp_from='thermal', T_ref=300)],
        loads=[dict(type='fixed_temperature', target='face:zmin', T=300),
               dict(type='convection', target='face:zmax', h=20, T_inf=400)],
        constraints=dict(rigid='3-2-1'),
        solver=dict(linear=dict(preconditioner='jacobi' if family == 'fvm' else 'direct',
                                rtol=1e-12, max_iter=2000)),
        output=dict(format='vtkhdf', warpage=dict(standard='JESD22-B112',
                                                remove_rigid='best_fit_plane')))


def solid_case(family='fvm'):
    case = thermal_case(family)
    case['physics'] = [dict(id='mech', solver=family + '.solid')]
    case['loads'] = [dict(type='uniform_temperature', T_ref=300, T_target=350)]
    return case


def fixture(root):
    root.mkdir(parents=True, exist_ok=True)
    stack = dict(id='fvm_synthetic', unit='m', symmetry='none', package_type='FCBGA',
                 outline=dict(box=[0, 0, 1, 1]), layers=[
                     dict(name=name, kind='Explicit', material=name, thickness=.05,
                          n_elem_min=1, grading=1) for name in ('a', 'b')])
    write_json(root / 'stack.json', stack)
    write_json(root / 'materials.json', dict(schema='chipcae.material_library/1',
                                            materials=[material('a', 2), material('b', 4)]))
    for ranks in (1, 2):
        write_json(root / f'profile{ranks}.json', dict(
            schema='chipcae.run_profile/1.0', backend='cpu', ranks=ranks, gpus=0))
    return stack


def measured_process(args, root):
    if not hasattr(os, 'wait4'):
        return subprocess.run(args, cwd=root, capture_output=True, text=True, timeout=500), None
    # Capture into files so neither pipe can block while wait4 obtains this child's
    # actual peak RSS. getrusage(RUSAGE_CHILDREN) would retain previous runs' peaks.
    with tempfile.TemporaryFile(mode='w+', encoding='utf-8') as stdout, \
            tempfile.TemporaryFile(mode='w+', encoding='utf-8') as stderr:
        with subprocess.Popen(args, cwd=root, stdout=stdout, stderr=stderr) as child:
            deadline = time.monotonic() + 500
            while True:
                pid, status, usage = os.wait4(child.pid, os.WNOHANG)
                if pid:
                    child.returncode = os.waitstatus_to_exitcode(status)
                    break
                if time.monotonic() > deadline:
                    child.kill()
                    child.wait()
                    raise subprocess.TimeoutExpired(args, 500)
                time.sleep(.01)
        stdout.seek(0)
        stderr.seek(0)
        result = subprocess.CompletedProcess(args, child.returncode, stdout.read(), stderr.read())
    rss = usage.ru_maxrss / 1024 if sys.platform == 'darwin' else usage.ru_maxrss
    return result, rss


def command(args, root, expected=0):
    args = [str(x) for x in args]
    print('COMMAND ' + shlex.join(args), flush=True)
    start = time.monotonic()
    try:
        result, rss = measured_process(args, root)
    except (OSError, subprocess.TimeoutExpired) as error:
        raise AssertionError(f'command could not complete: {args!r}: {error}') from error
    print(f'实测 wall={time.monotonic() - start:.6f}s peak_rss_KiB={rss} '
          f'exit={result.returncode}', flush=True)
    if expected is not None:
        assert result.returncode == expected, (
            f'{shlex.join(args)}\nexit={result.returncode}, expected={expected}\n'
            f'stdout:\n{result.stdout}\nstderr:\n{result.stderr}')
    return result


def probe(root, case, expected=0, ranks=1):
    write_json(root / 'case.json', case)
    result = command([os.environ['CHIPCAE_FVM_LOADER_PROBE'], root / 'case.json',
                      root / f'profile{ranks}.json', ranks], root, expected)
    return (json.loads(result.stdout) if result.returncode == 0 else None), result


def normalize_paths(value, root):
    if isinstance(value, dict):
        return {k: normalize_paths(v, root) for k, v in value.items()}
    if isinstance(value, list):
        return [normalize_paths(v, root) for v in value]
    return value.replace(str(root.resolve()), '<fixture>') if isinstance(value, str) else value


def snapshot_fixture(root, name):
    stack = fixture(root)
    if name == 'copper':
        stack['layers'][0].update(kind='Patterned', copper=dict(metal='cu', fraction=.5))
        stack['layers'][1]['regions'] = [dict(
            name='patch', material='a', footprint=dict(box=[.25, .25, .75, .75]))]
        library = read_json(root / 'materials.json')
        library['materials'].append(material('cu', 400))
        write_json(root / 'materials.json', library)
        write_json(root / 'stack.json', stack)


def mpi_command(args, ranks):
    if ranks == 1:
        return args
    pre = [x for x in os.environ.get('CHIPCAE_MPIEXEC_PREFLAGS', '').split('|') if x]
    post = [x for x in os.environ.get('CHIPCAE_MPIEXEC_POSTFLAGS', '').split('|') if x]
    return [os.environ['CHIPCAE_MPIEXEC'],
            os.environ['CHIPCAE_MPIEXEC_NUMPROC_FLAG'], str(ranks), *pre, *args, *post]


def solve(root, case, name, ranks=1, extra=(), control=None, expected=0):
    write_json(root / 'case.json', case)
    out = root / name
    out.mkdir(exist_ok=True)
    if control is not None:
        (out / 'control.jsonl').write_text(control, encoding='utf-8')
    args = [os.environ['CHIPCAE_SOLVE_EXE'], '--case', root / 'case.json', '--profile',
            root / f'profile{ranks}.json', '--out', out, *extra]
    return out, command(mpi_command(args, ranks), root, expected)


class Cursor:
    def __init__(self, data):
        self.data, self.pos = data, 0

    def take(self, fmt):
        size = struct.calcsize('<' + fmt)
        assert self.pos + size <= len(self.data), 'truncated event payload'
        values = struct.unpack_from('<' + fmt, self.data, self.pos)
        self.pos += size
        return values[0] if len(values) == 1 else values

    def string(self):
        size = self.take('I')
        assert self.pos + size <= len(self.data), 'truncated event string'
        data = self.data[self.pos:self.pos + size]
        self.pos += size
        try:
            return data.decode('utf-8', errors='strict')
        except UnicodeDecodeError as error:
            raise AssertionError('invalid UTF-8 in event') from error

    def strings(self):
        return [self.string() for _ in range(self.take('I'))]


def decode_events(data):
    """Reject corrupt or partial completed logs, including payload trailing bytes."""
    result, pos, previous = [], 0, 0
    while pos < len(data):
        assert pos + 16 <= len(data), 'truncated event header'
        assert data[pos:pos + 8] == b'CAELOG1\0', 'bad event magic'
        size, crc = struct.unpack_from('<II', data, pos + 8)
        assert 17 <= size <= 64 * 1024 * 1024, 'invalid event length'
        body = data[pos + 16:pos + 16 + size]
        assert len(body) == size, 'truncated event record'
        assert zlib.crc32(body) == crc, 'event CRC mismatch'
        c = Cursor(body)
        seq, wall, kind = c.take('QqB')
        assert seq > previous, 'event sequence must increase'
        previous = seq
        e = dict(seq=seq, wall=wall, kind=kind)
        if kind == 20:
            e['level'], e['rank'] = c.take('ii')
            e['text'], e['code'], e['error_code'] = c.string(), c.string(), c.take('I')
        elif kind == 19:
            e['exit_code'], e['last_window'] = c.take('iQ')
            e['reason'] = c.string()
        elif kind == 21:
            e['state'], e['window'], e['time'] = c.take('BQd')
            assert 0 <= e['state'] <= 5, 'invalid state'
        elif kind == 22:
            e['window'], e['time'] = c.take('Qd')
            e['path'], e['checksum'] = c.string(), c.string()
        elif kind == 17:
            e['cmd_seq'], e['request'] = c.take('Q'), c.string()
            e['status'], e['step'] = c.take('IQ')
            e['text'], e['time'] = c.string(), c.take('d')
        elif kind == 18:
            e['window'], e['time'] = c.take('Qd')
            e['points'] = []
            for _ in range(c.take('I')):
                solver, field = c.string(), c.string()
                iteration, norm, flags, linear, window = c.take('IdIII')
                e['points'].append(dict(solver=solver, field=field, iteration=iteration,
                                        norm=norm, flags=flags, linear=linear, window=window))
            e['dropped'] = c.take('I')
        elif kind == 11:
            e['solver'], e['field'] = c.string(), c.string()
            e['window'], e['iteration'], e['norm'], e['flags'] = c.take('QIdI')
            e['code'], e['error_code'] = c.string(), c.take('I')
        elif kind == 10:
            c.take('Qddd')
            c.string()
        elif kind == 15:
            c.string()
            c.take('I')
            c.strings()
            c.string()
            c.string()
            for _ in range(c.take('I')):
                c.string()
                c.string()
            c.take('II')
            for _ in range(c.take('I')):
                c.take('I')
            for _ in range(3):
                c.string()
            c.strings()
        else:
            raise AssertionError(f'unknown event kind {kind}')
        assert c.pos == len(body), 'event payload has trailing bytes'
        result.append(e)
        pos += 16 + size
    assert result, 'empty event log'
    return result


def energy(events):
    records = [e for e in events if e['kind'] == 20 and e['text'].startswith(ENERGY_PREFIX)]
    assert len(records) == 1, f'expected exactly one FVM_ENERGY LogLine, got {len(records)}'
    event = records[0]
    assert event['level'] == 1 and event['rank'] == 0, event
    def pairs(items):
        assert len(items) == len({k for k, _ in items}), 'duplicate energy JSON keys'
        return dict(items)
    def constant(text):
        raise AssertionError(f'nonfinite JSON constant {text}')
    text = event['text'][len(ENERGY_PREFIX):]
    assert '\n' not in text and '\r' not in text, 'energy JSON must be one line'
    try:
        value = json.loads(text, object_pairs_hook=pairs, parse_constant=constant)
    except (ValueError, TypeError) as error:
        raise AssertionError(f'malformed energy JSON: {text!r}') from error
    assert isinstance(value, dict) and set(value) == {
        'heat_in', 'heat_out', 'imbalance', 'iterations'}, value
    for key in ('heat_in', 'heat_out', 'imbalance'):
        assert type(value[key]) in (int, float), (key, value)
        assert math.isfinite(value[key]) and value[key] >= 0, (key, value)
    assert type(value['iterations']) is int and value['iterations'] >= 0, value
    recomputed = abs(value['heat_in'] - value['heat_out']) / max(
        value['heat_in'] + value['heat_out'], 1e-300)
    assert math.isfinite(recomputed), value
    assert abs(value['imbalance'] - recomputed) <= max(1e-15, 1e-12 * abs(recomputed)), value
    assert value['imbalance'] <= 1e-9 and recomputed <= 1e-9, (value, recomputed)
    return value


def read_result(out, require_volume=True):
    import h5py
    import numpy as np
    manifest = read_json(out / 'manifest.json')
    assert manifest['schema_version'] == 'chipcae.results/1.0', manifest
    digest = manifest['mesh']['mesh_sha256']
    assert len(digest) == 64 and all(c in '0123456789abcdef' for c in digest), digest
    assert len(manifest['steps']) == 1, manifest['steps']
    step = out / manifest['steps'][0]['file']
    assert step.is_file() and step.stat().st_size > 0, step
    blocks = {}
    with h5py.File(step, 'r') as f:
        for name in ('hex', 'cut', 'eb_surface'):
            g = f['VTKHDF/' + name]
            data = {}
            g.visititems(lambda key, obj: data.update({key: obj[:]})
                         if isinstance(obj, h5py.Dataset) else None)
            blocks[name] = data
            points = data['Points']
            assert points.ndim == 2 and points.shape[1] == 3 and np.all(np.isfinite(points))
            assert data['PointData/T'].shape == (len(points),)
            assert data['PointData/U'].shape == (len(points), 3)
            assert np.all(np.isfinite(data['PointData/T']))
            assert np.all(np.isfinite(data['PointData/U']))
            if name == 'eb_surface':
                count = int(data['Polygons/NumberOfCells'].sum())
                assert data['Polygons/Connectivity'].size == 3 * count
                for field in ('layer_id', 'material_id', 'cell_kind', 'elem_gid'):
                    a = data['CellData/' + field]
                    assert a.shape == (count,) and np.issubdtype(a.dtype, np.integer), (name, field)
                continue
            count = int(data['NumberOfCells'].sum())
            assert len(data['Types']) == count
            assert np.all(data['Types'] == (12 if name == 'hex' else 42)), name
            for field in ('layer_id', 'material_id', 'cell_kind', 'elem_gid'):
                a = data['CellData/' + field]
                assert a.shape == (count,) and np.issubdtype(a.dtype, np.integer), (name, field)
            if name == 'cut':
                assert np.all(data['CellData/cell_kind'] == 1), name
            else:
                assert np.all(np.isin(data['CellData/cell_kind'], [0, 1])), name
            if require_volume and count:
                volumes = data['CellData/volume']
                assert volumes.shape == (count,) and np.all(np.isfinite(volumes))
                assert np.all(volumes > 0), (name, volumes)
            for optional, width in (('copper_fraction', 1), ('conductivity_diag', 3)):
                key = 'CellData/' + optional
                if require_volume and REQUIRE_COPPER_FIELDS and count:
                    assert key in data, f'{name}: missing required upstream field {optional}'
                if key in data:
                    shape = (count,) if width == 1 else (count, width)
                    assert data[key].shape == shape and np.all(np.isfinite(data[key]))
    return manifest, blocks


def canonical_cells(block):
    """Key by parent gid and cell's coordinates, interpreting each rank's local IDs."""
    cells, p0, c0, o0, k0 = {}, 0, 0, 0, 0
    counts = zip(block['NumberOfPoints'], block['NumberOfCells'],
                 block['NumberOfConnectivityIds'], strict=True)
    for npoints, ncells, nconn in counts:
        npoints, ncells, nconn = int(npoints), int(ncells), int(nconn)
        offsets = block['Offsets'][o0:o0 + ncells + 1]
        conn = block['Connectivity'][k0:k0 + nconn]
        assert len(offsets) == ncells + 1 and offsets[0] == 0 and offsets[-1] == nconn
        for local in range(ncells):
            indices = conn[int(offsets[local]):int(offsets[local + 1])].astype(int)
            assert len(indices) > 0 and min(indices) >= 0 and max(indices) < npoints
            pairs = sorted((tuple(block['Points'][p0 + i]),
                            float(block['PointData/T'][p0 + i])) for i in indices)
            key = (int(block['CellData/elem_gid'][c0 + local]), tuple(p for p, _ in pairs))
            assert key not in cells, f'duplicate owned volume cell {key}'
            cells[key] = tuple(t for _, t in pairs)
        p0, c0, o0, k0 = p0 + npoints, c0 + ncells, o0 + ncells + 1, k0 + nconn
    assert p0 == len(block['Points']) and c0 == len(block['Types'])
    assert o0 == len(block['Offsets']) and k0 == len(block['Connectivity'])
    return cells


def compare_parallel(first, second):
    import numpy as np
    max_error = 0.0
    for name in ('hex', 'cut'):
        a, b = canonical_cells(first[name]), canonical_cells(second[name])
        assert a.keys() == b.keys(), f'{name}: owned gid/coordinate sets differ'
        for key in a:
            x, y = np.asarray(a[key]), np.asarray(b[key])
            error = float(np.max(np.abs(x - y))) / max(float(np.max(np.abs(x))), 1e-300)
            max_error = max(max_error, error)
            assert error <= 1e-8, (name, key, error)
    print(f'实测 compare_parallel maximum_relative_difference={max_error:.17g}', flush=True)


def inspect_fvm(out, blocks, manifest, stopped=False):
    import numpy as np
    events = decode_events((out / 'events.bin').read_bytes())
    ledger = energy(events)
    assert events[-1]['kind'] == 19 and events[-1]['exit_code'] == 0
    assert events[-1]['reason'] == ('user_stop' if stopped else 'end_reached')
    points = [p for e in events if e['kind'] == 18 for p in e['points']
              if p['solver'] == 'fvm.conduction']
    assert points, 'missing fvm.conduction ResidualBatch'
    assert points[-1]['iteration'] == ledger['iterations'], (points[-1], ledger)
    assert math.isfinite(points[-1]['norm']) and points[-1]['norm'] >= 0
    if stopped:
        assert manifest['steps'] == []
        assert not any(e['kind'] == 22 for e in events)
        return ledger, events
    summaries = {x['name']: x for x in manifest['summary']}
    assert summaries['T_max']['method'] == 'FVM cell centers'
    assert math.isfinite(summaries['T_max']['value'])
    for name in ('warpage_pv', 'U_mag_max'):
        assert abs(summaries[name]['value']) <= 1e-15, summaries[name]
        assert summaries[name]['method'] == THERMAL_ONLY
    assert any(e['kind'] == 20 and e['level'] == 2 and e['text'] == THERMAL_ONLY for e in events)
    for name, data in blocks.items():
        assert np.allclose(data['PointData/U'], 0, rtol=0, atol=1e-15), name
    states = [e['state'] for e in events if e['kind'] == 21]
    assert states[0] == 1 and states[-1] == 4 and 2 in states, states
    outputs = [e for e in events if e['kind'] == 22]
    assert len(outputs) == 1
    actual = hashlib.sha256((out / outputs[0]['path']).read_bytes()).hexdigest()
    assert outputs[0]['checksum'] == actual
    return ledger, events


def summary(manifest):
    return {x['name']: x['value'] for x in manifest['summary']}


def import_board(root, source):
    fixture(root)
    command([os.environ['CHIPCAE_IMPORT_EXE'],
             source / 'tests/integration/ecad/data/board_4layer.xml',
             '-o', 'stack.json', '--polyset', 'stack.polyset', '--report', 'defects.json',
             '--material-map', source / 'tests/integration/f1130_uncut_cli/material_map.json'], root)
    # Deliberately use import artifacts unchanged: fraction averaging hides the prerequisite.
    stack = read_json(root / 'stack.json')
    assert any('polygons' in l.get('copper', {}) for l in stack['layers'])
    materials = [material('cu', 390), material('fr4_eq', .3)]
    write_json(root / 'materials.json', dict(schema='chipcae.material_library/1', materials=materials))
    originals = {p: (root / p).read_bytes() for p in ('stack.json', 'stack.polyset')}
    result = command([os.environ['CHIPCAE_PREVIEW_EXE'], root / 'stack.json',
                      '-o', root / 'preview.vtkhdf', '--nx', '4', '--ny', '4',
                      '--levels', '0', '--max-ratio', '100'], root)
    successes = [e for line in result.stdout.splitlines() if line.strip()
                 for e in [json.loads(line)] if e.get('stage') == 'success']
    assert len(successes) == 1, result.stdout
    report = successes[0]
    assert type(report['cell_count']) is int and report['cell_count'] > 0, report
    assert (root / 'preview.vtkhdf').is_file()
    return stack, report, originals


def copy_demo(root, source):
    shutil.copytree(source / 'examples/demo_gate', root, dirs_exist_ok=True)
    for ranks in (1, 2):
        profile = read_json(root / 'profile.json')
        profile['ranks'] = ranks
        write_json(root / f'profile{ranks}.json', profile)
    return read_json(root / 'case.json')


def wiener_fixture(root):
    stack = fixture(root)
    sidecar = root / 'half.polyset'
    command([os.environ['CHIPCAE_FVM_LOADER_PROBE'], '--half-polyset', sidecar], root)
    stack['layers'][0].update(kind='Patterned', copper=dict(
        metal='cu', polygons=dict(file=sidecar.name, layer='trace')))
    stack['external_refs'] = [dict(path=sidecar.name,
                                  sha256=hashlib.sha256(sidecar.read_bytes()).hexdigest())]
    library = read_json(root / 'materials.json')
    library['materials'].append(material('cu', 400))
    write_json(root / 'materials.json', library)
    write_json(root / 'stack.json', stack)
    return {name: (root / name).read_bytes() for name in ('stack.json', 'half.polyset')}


# Frozen at upstream merge 75878ee (origin/main c9c1464); only temporary paths normalized.
FEM_SNAPSHOTS = {'copper': {'T_ref': 300.0,
            'backend': 'cpu',
            'case_path': '<fixture>/case.json',
            'cut_as_hex': False,
            'cut_options': {'export_polyhedra': False,
                            'keep_void': False,
                            'quad_degree': 6,
                            'small_sub_vf': 1e-08,
                            'snap_phi': 1e-14},
            'fields': ['T', 'U', 'layer_id', 'material_id', 'cell_kind', 'elem_gid',
                       'copper_fraction', 'conductivity_diag'],
            'fixed': [],
            'ghost_penalty': 0.1,
            'grid': {'ar_max': 'inf',
                     'ar_warn': 'inf',
                     'cell_budget': 50000,
                     'levels': 0,
                     'max_ratio': 100.0,
                     'nx': 4,
                     'ny': 4,
                     'ranks': 1,
                     'snap_tol': 1e-14,
                     'use_metis': False},
            'linear': {'max_iter': 2000, 'preconditioner': 'direct', 'rtol': 1e-12},
            'loads': [{'T': 300.0,
                       'T_inf': 0.0,
                       'T_ref': 0.0,
                       'h': 0.0,
                       'id': '',
                       'source': 0.0,
                       'target': 'face:zmin',
                       'total_power': False,
                       'type': 'fixed_temperature'},
                      {'T': 0.0,
                       'T_inf': 400.0,
                       'T_ref': 0.0,
                       'h': 20.0,
                       'id': '',
                       'source': 0.0,
                       'target': 'face:zmax',
                       'total_power': False,
                       'type': 'convection'}],
            'material_path': '<fixture>/materials.json',
            'materials': {'a': {'E': 25000000000.0,
                                'E_clamped': False,
                                'E_table': {'extrapolation': 'clamp',
                                            'interpolation': 'constant',
                                            'temperatures': [300.0],
                                            'values': [25000000000.0]},
                                'alpha': 1e-05,
                                'alpha_clamped': False,
                                'alpha_table': {'extrapolation': 'clamp',
                                                'interpolation': 'constant',
                                                'temperatures': [300.0],
                                                'values': [1e-05]},
                                'id': 'a',
                                'k': 2.0,
                                'nu': 0.3,
                                'nu_clamped': False,
                                'nu_table': {'extrapolation': 'clamp',
                                             'interpolation': 'constant',
                                             'temperatures': [300.0],
                                             'values': [0.3]}},
                          'b': {'E': 25000000000.0,
                                'E_clamped': False,
                                'E_table': {'extrapolation': 'clamp',
                                            'interpolation': 'constant',
                                            'temperatures': [300.0],
                                            'values': [25000000000.0]},
                                'alpha': 1e-05,
                                'alpha_clamped': False,
                                'alpha_table': {'extrapolation': 'clamp',
                                                'interpolation': 'constant',
                                                'temperatures': [300.0],
                                                'values': [1e-05]},
                                'id': 'b',
                                'k': 4.0,
                                'nu': 0.3,
                                'nu_clamped': False,
                                'nu_table': {'extrapolation': 'clamp',
                                             'interpolation': 'constant',
                                             'temperatures': [300.0],
                                             'values': [0.3]}},
                          'cu': {'E': 25000000000.0,
                                 'E_clamped': False,
                                 'E_table': {'extrapolation': 'clamp',
                                             'interpolation': 'constant',
                                             'temperatures': [300.0],
                                             'values': [25000000000.0]},
                                 'alpha': 1e-05,
                                 'alpha_clamped': False,
                                 'alpha_table': {'extrapolation': 'clamp',
                                                 'interpolation': 'constant',
                                                 'temperatures': [300.0],
                                                 'values': [1e-05]},
                                 'id': 'cu',
                                 'k': 400.0,
                                 'nu': 0.3,
                                 'nu_clamped': False,
                                 'nu_table': {'extrapolation': 'clamp',
                                              'interpolation': 'constant',
                                              'temperatures': [300.0],
                                              'values': [0.3]}}},
            'profile_path': '<fixture>/profile1.json',
            'stack': {'activation_groups': {},
                      'id': 'fvm_synthetic',
                      'layers': [{'body': 'a',
                                  'copper': {'fraction': 0.5, 'metal': 'cu', 'polygons': None},
                                  'grading': 1.0,
                                  'kind': 'Patterned',
                                  'material': 'a',
                                  'n_elem_min': 1,
                                  'name': 'a',
                                  'regions': [],
                                  'thickness': 0.05},
                                 {'body': 'b',
                                  'copper': None,
                                  'grading': 1.0,
                                  'kind': 'Explicit',
                                  'material': 'b',
                                  'n_elem_min': 1,
                                  'name': 'b',
                                  'regions': [{'body': 'patch',
                                               'footprint': {'box': [0.25, 0.25, 0.75, 0.75],
                                                             'polygon': []},
                                               'material': 'a',
                                               'name': 'patch'}],
                                  'thickness': 0.05}],
                      'outline': {'box': [0.0, 0.0, 1.0, 1.0], 'polygon': []},
                      'package_type': 'FCBGA',
                      'symmetry': 'none',
                      'unit': 'm',
                      'warnings': []},
            'stack_path': '<fixture>/stack.json',
            'thermal': True},
 'custom': {'T_ref': 300.0,
            'backend': 'cpu',
            'case_path': '<fixture>/case.json',
            'cut_as_hex': False,
            'cut_options': {'export_polyhedra': False,
                            'keep_void': False,
                            'quad_degree': 4,
                            'small_sub_vf': 1e-08,
                            'snap_phi': 1e-14},
            'fields': ['T', 'U', 'elem_gid'],
            'fixed': ['face:xmin'],
            'ghost_penalty': 0.25,
            'grid': {'ar_max': 'inf',
                     'ar_warn': 'inf',
                     'cell_budget': 12345,
                     'levels': 2,
                     'max_ratio': 100.0,
                     'nx': 3,
                     'ny': 5,
                     'ranks': 1,
                     'snap_tol': 1e-14,
                     'use_metis': False},
            'linear': {'max_iter': 321, 'preconditioner': 'column_gmg', 'rtol': 2e-09},
            'loads': [{'T': 300.0,
                       'T_inf': 0.0,
                       'T_ref': 0.0,
                       'h': 0.0,
                       'id': '',
                       'source': 0.0,
                       'target': 'face:zmin',
                       'total_power': False,
                       'type': 'fixed_temperature'},
                      {'T': 0.0,
                       'T_inf': 400.0,
                       'T_ref': 0.0,
                       'h': 20.0,
                       'id': '',
                       'source': 0.0,
                       'target': 'face:zmax',
                       'total_power': False,
                       'type': 'convection'}],
            'material_path': '<fixture>/materials.json',
            'materials': {'a': {'E': 25000000000.0,
                                'E_clamped': False,
                                'E_table': {'extrapolation': 'clamp',
                                            'interpolation': 'constant',
                                            'temperatures': [300.0],
                                            'values': [25000000000.0]},
                                'alpha': 1e-05,
                                'alpha_clamped': False,
                                'alpha_table': {'extrapolation': 'clamp',
                                                'interpolation': 'constant',
                                                'temperatures': [300.0],
                                                'values': [1e-05]},
                                'id': 'a',
                                'k': 2.0,
                                'nu': 0.3,
                                'nu_clamped': False,
                                'nu_table': {'extrapolation': 'clamp',
                                             'interpolation': 'constant',
                                             'temperatures': [300.0],
                                             'values': [0.3]}},
                          'b': {'E': 25000000000.0,
                                'E_clamped': False,
                                'E_table': {'extrapolation': 'clamp',
                                            'interpolation': 'constant',
                                            'temperatures': [300.0],
                                            'values': [25000000000.0]},
                                'alpha': 1e-05,
                                'alpha_clamped': False,
                                'alpha_table': {'extrapolation': 'clamp',
                                                'interpolation': 'constant',
                                                'temperatures': [300.0],
                                                'values': [1e-05]},
                                'id': 'b',
                                'k': 4.0,
                                'nu': 0.3,
                                'nu_clamped': False,
                                'nu_table': {'extrapolation': 'clamp',
                                             'interpolation': 'constant',
                                             'temperatures': [300.0],
                                             'values': [0.3]}}},
            'profile_path': '<fixture>/profile1.json',
            'stack': {'activation_groups': {},
                      'id': 'fvm_synthetic',
                      'layers': [{'body': 'a',
                                  'copper': None,
                                  'grading': 1.0,
                                  'kind': 'Explicit',
                                  'material': 'a',
                                  'n_elem_min': 1,
                                  'name': 'a',
                                  'regions': [],
                                  'thickness': 0.05},
                                 {'body': 'b',
                                  'copper': None,
                                  'grading': 1.0,
                                  'kind': 'Explicit',
                                  'material': 'b',
                                  'n_elem_min': 1,
                                  'name': 'b',
                                  'regions': [],
                                  'thickness': 0.05}],
                      'outline': {'box': [0.0, 0.0, 1.0, 1.0], 'polygon': []},
                      'package_type': 'FCBGA',
                      'symmetry': 'none',
                      'unit': 'm',
                      'warnings': []},
            'stack_path': '<fixture>/stack.json',
            'thermal': True},
 'default': {'T_ref': 300.0,
             'backend': 'cpu',
             'case_path': '<fixture>/case.json',
             'cut_as_hex': False,
             'cut_options': {'export_polyhedra': False,
                             'keep_void': False,
                             'quad_degree': 6,
                             'small_sub_vf': 1e-08,
                             'snap_phi': 1e-14},
             'fields': ['T', 'U', 'layer_id', 'material_id', 'cell_kind', 'elem_gid',
                        'copper_fraction', 'conductivity_diag'],
             'fixed': [],
             'ghost_penalty': 0.1,
             'grid': {'ar_max': 'inf',
                      'ar_warn': 'inf',
                      'cell_budget': 50000,
                      'levels': 0,
                      'max_ratio': 100.0,
                      'nx': 4,
                      'ny': 4,
                      'ranks': 1,
                      'snap_tol': 1e-14,
                      'use_metis': False},
             'linear': {'max_iter': 500, 'preconditioner': 'column_gmg', 'rtol': 1e-08},
             'loads': [{'T': 300.0,
                        'T_inf': 0.0,
                        'T_ref': 0.0,
                        'h': 0.0,
                        'id': '',
                        'source': 0.0,
                        'target': 'face:zmin',
                        'total_power': False,
                        'type': 'fixed_temperature'},
                       {'T': 0.0,
                        'T_inf': 400.0,
                        'T_ref': 0.0,
                        'h': 20.0,
                        'id': '',
                        'source': 0.0,
                        'target': 'face:zmax',
                        'total_power': False,
                        'type': 'convection'}],
             'material_path': '<fixture>/materials.json',
             'materials': {'a': {'E': 25000000000.0,
                                 'E_clamped': False,
                                 'E_table': {'extrapolation': 'clamp',
                                             'interpolation': 'constant',
                                             'temperatures': [300.0],
                                             'values': [25000000000.0]},
                                 'alpha': 1e-05,
                                 'alpha_clamped': False,
                                 'alpha_table': {'extrapolation': 'clamp',
                                                 'interpolation': 'constant',
                                                 'temperatures': [300.0],
                                                 'values': [1e-05]},
                                 'id': 'a',
                                 'k': 2.0,
                                 'nu': 0.3,
                                 'nu_clamped': False,
                                 'nu_table': {'extrapolation': 'clamp',
                                              'interpolation': 'constant',
                                              'temperatures': [300.0],
                                              'values': [0.3]}},
                           'b': {'E': 25000000000.0,
                                 'E_clamped': False,
                                 'E_table': {'extrapolation': 'clamp',
                                             'interpolation': 'constant',
                                             'temperatures': [300.0],
                                             'values': [25000000000.0]},
                                 'alpha': 1e-05,
                                 'alpha_clamped': False,
                                 'alpha_table': {'extrapolation': 'clamp',
                                                 'interpolation': 'constant',
                                                 'temperatures': [300.0],
                                                 'values': [1e-05]},
                                 'id': 'b',
                                 'k': 4.0,
                                 'nu': 0.3,
                                 'nu_clamped': False,
                                 'nu_table': {'extrapolation': 'clamp',
                                              'interpolation': 'constant',
                                              'temperatures': [300.0],
                                              'values': [0.3]}}},
             'profile_path': '<fixture>/profile1.json',
             'stack': {'activation_groups': {},
                       'id': 'fvm_synthetic',
                       'layers': [{'body': 'a',
                                   'copper': None,
                                   'grading': 1.0,
                                   'kind': 'Explicit',
                                   'material': 'a',
                                   'n_elem_min': 1,
                                   'name': 'a',
                                   'regions': [],
                                   'thickness': 0.05},
                                  {'body': 'b',
                                   'copper': None,
                                   'grading': 1.0,
                                   'kind': 'Explicit',
                                   'material': 'b',
                                   'n_elem_min': 1,
                                   'name': 'b',
                                   'regions': [],
                                   'thickness': 0.05}],
                       'outline': {'box': [0.0, 0.0, 1.0, 1.0], 'polygon': []},
                       'package_type': 'FCBGA',
                       'symmetry': 'none',
                       'unit': 'm',
                       'warnings': []},
             'stack_path': '<fixture>/stack.json',
             'thermal': True},
 'solid': {'T_ref': 300.0,
           'backend': 'cpu',
           'case_path': '<fixture>/case.json',
           'cut_as_hex': False,
           'cut_options': {'export_polyhedra': False,
                           'keep_void': False,
                           'quad_degree': 6,
                           'small_sub_vf': 1e-08,
                           'snap_phi': 1e-14},
           'fields': ['T', 'U', 'layer_id', 'material_id', 'cell_kind', 'elem_gid',
                      'copper_fraction', 'conductivity_diag'],
           'fixed': [],
           'ghost_penalty': 0.1,
           'grid': {'ar_max': 'inf',
                    'ar_warn': 'inf',
                    'cell_budget': 50000,
                    'levels': 0,
                    'max_ratio': 100.0,
                    'nx': 4,
                    'ny': 4,
                    'ranks': 1,
                    'snap_tol': 1e-14,
                    'use_metis': False},
           'linear': {'max_iter': 2000, 'preconditioner': 'direct', 'rtol': 1e-12},
           'loads': [{'T': 350.0,
                      'T_inf': 0.0,
                      'T_ref': 300.0,
                      'h': 0.0,
                      'id': '',
                      'source': 0.0,
                      'target': '',
                      'total_power': False,
                      'type': 'uniform_temperature'}],
           'material_path': '<fixture>/materials.json',
           'materials': {'a': {'E': 25000000000.0,
                               'E_clamped': False,
                               'E_table': {'extrapolation': 'clamp',
                                           'interpolation': 'constant',
                                           'temperatures': [300.0],
                                           'values': [25000000000.0]},
                               'alpha': 1e-05,
                               'alpha_clamped': False,
                               'alpha_table': {'extrapolation': 'clamp',
                                               'interpolation': 'constant',
                                               'temperatures': [300.0],
                                               'values': [1e-05]},
                               'id': 'a',
                               'k': 2.0,
                               'nu': 0.3,
                               'nu_clamped': False,
                               'nu_table': {'extrapolation': 'clamp',
                                            'interpolation': 'constant',
                                            'temperatures': [300.0],
                                            'values': [0.3]}},
                         'b': {'E': 25000000000.0,
                               'E_clamped': False,
                               'E_table': {'extrapolation': 'clamp',
                                           'interpolation': 'constant',
                                           'temperatures': [300.0],
                                           'values': [25000000000.0]},
                               'alpha': 1e-05,
                               'alpha_clamped': False,
                               'alpha_table': {'extrapolation': 'clamp',
                                               'interpolation': 'constant',
                                               'temperatures': [300.0],
                                               'values': [1e-05]},
                               'id': 'b',
                               'k': 4.0,
                               'nu': 0.3,
                               'nu_clamped': False,
                               'nu_table': {'extrapolation': 'clamp',
                                            'interpolation': 'constant',
                                            'temperatures': [300.0],
                                            'values': [0.3]}}},
           'profile_path': '<fixture>/profile1.json',
           'stack': {'activation_groups': {},
                     'id': 'fvm_synthetic',
                     'layers': [{'body': 'a',
                                 'copper': None,
                                 'grading': 1.0,
                                 'kind': 'Explicit',
                                 'material': 'a',
                                 'n_elem_min': 1,
                                 'name': 'a',
                                 'regions': [],
                                 'thickness': 0.05},
                                {'body': 'b',
                                 'copper': None,
                                 'grading': 1.0,
                                 'kind': 'Explicit',
                                 'material': 'b',
                                 'n_elem_min': 1,
                                 'name': 'b',
                                 'regions': [],
                                 'thickness': 0.05}],
                     'outline': {'box': [0.0, 0.0, 1.0, 1.0], 'polygon': []},
                     'package_type': 'FCBGA',
                     'symmetry': 'none',
                     'unit': 'm',
                     'warnings': []},
           'stack_path': '<fixture>/stack.json',
           'thermal': False},
 'sources': {'T_ref': 300.0,
             'backend': 'cpu',
             'case_path': '<fixture>/case.json',
             'cut_as_hex': False,
             'cut_options': {'export_polyhedra': False,
                             'keep_void': False,
                             'quad_degree': 6,
                             'small_sub_vf': 1e-08,
                             'snap_phi': 1e-14},
             'fields': ['T', 'U', 'layer_id', 'material_id', 'cell_kind', 'elem_gid',
                        'copper_fraction', 'conductivity_diag'],
             'fixed': [],
             'ghost_penalty': 0.1,
             'grid': {'ar_max': 'inf',
                      'ar_warn': 'inf',
                      'cell_budget': 50000,
                      'levels': 0,
                      'max_ratio': 100.0,
                      'nx': 4,
                      'ny': 4,
                      'ranks': 1,
                      'snap_tol': 1e-14,
                      'use_metis': False},
             'linear': {'max_iter': 2000, 'preconditioner': 'direct', 'rtol': 1e-12},
             'loads': [{'T': 350.0,
                        'T_inf': 0.0,
                        'T_ref': 0.0,
                        'h': 0.0,
                        'id': 'pin',
                        'source': 0.0,
                        'target': 'layer:a',
                        'total_power': False,
                        'type': 'fixed_temperature'},
                       {'T': 0.0,
                        'T_inf': 300.0,
                        'T_ref': 0.0,
                        'h': 20.0,
                        'id': 'cool',
                        'source': 0.0,
                        'target': 'layer:b',
                        'total_power': False,
                        'type': 'convection'},
                       {'T': 0.0,
                        'T_inf': 0.0,
                        'T_ref': 0.0,
                        'h': 0.0,
                        'id': 'power',
                        'source': 7.0,
                        'target': 'layer:a',
                        'total_power': True,
                        'type': 'heat_source'},
                       {'T': 0.0,
                        'T_inf': 0.0,
                        'T_ref': 0.0,
                        'h': 0.0,
                        'id': 'density',
                        'source': 3.0,
                        'target': 'layer:b',
                        'total_power': False,
                        'type': 'heat_source'}],
             'material_path': '<fixture>/materials.json',
             'materials': {'a': {'E': 25000000000.0,
                                 'E_clamped': False,
                                 'E_table': {'extrapolation': 'clamp',
                                             'interpolation': 'constant',
                                             'temperatures': [300.0],
                                             'values': [25000000000.0]},
                                 'alpha': 1e-05,
                                 'alpha_clamped': False,
                                 'alpha_table': {'extrapolation': 'clamp',
                                                 'interpolation': 'constant',
                                                 'temperatures': [300.0],
                                                 'values': [1e-05]},
                                 'id': 'a',
                                 'k': 2.0,
                                 'nu': 0.3,
                                 'nu_clamped': False,
                                 'nu_table': {'extrapolation': 'clamp',
                                              'interpolation': 'constant',
                                              'temperatures': [300.0],
                                              'values': [0.3]}},
                           'b': {'E': 25000000000.0,
                                 'E_clamped': False,
                                 'E_table': {'extrapolation': 'clamp',
                                             'interpolation': 'constant',
                                             'temperatures': [300.0],
                                             'values': [25000000000.0]},
                                 'alpha': 1e-05,
                                 'alpha_clamped': False,
                                 'alpha_table': {'extrapolation': 'clamp',
                                                 'interpolation': 'constant',
                                                 'temperatures': [300.0],
                                                 'values': [1e-05]},
                                 'id': 'b',
                                 'k': 4.0,
                                 'nu': 0.3,
                                 'nu_clamped': False,
                                 'nu_table': {'extrapolation': 'clamp',
                                              'interpolation': 'constant',
                                              'temperatures': [300.0],
                                              'values': [0.3]}}},
             'profile_path': '<fixture>/profile1.json',
             'stack': {'activation_groups': {},
                       'id': 'fvm_synthetic',
                       'layers': [{'body': 'a',
                                   'copper': None,
                                   'grading': 1.0,
                                   'kind': 'Explicit',
                                   'material': 'a',
                                   'n_elem_min': 1,
                                   'name': 'a',
                                   'regions': [],
                                   'thickness': 0.05},
                                  {'body': 'b',
                                   'copper': None,
                                   'grading': 1.0,
                                   'kind': 'Explicit',
                                   'material': 'b',
                                   'n_elem_min': 1,
                                   'name': 'b',
                                   'regions': [],
                                   'thickness': 0.05}],
                       'outline': {'box': [0.0, 0.0, 1.0, 1.0], 'polygon': []},
                       'package_type': 'FCBGA',
                       'symmetry': 'none',
                       'unit': 'm',
                       'warnings': []},
             'stack_path': '<fixture>/stack.json',
             'thermal': True}}
