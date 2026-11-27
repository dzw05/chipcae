"""K1 acceptance on generated FEM strips; expectations are analytic, never fixtures."""
import importlib.util
import json
import math
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import time

try:
    import h5py
    import numpy as np
except ImportError as error:
    print(f'FAIL: K1 acceptance requires numpy and h5py: {error}', file=sys.stderr)
    sys.exit(1)

EXE, MPIEXEC, SOURCE_ARG = sys.argv[1:]
SOURCE = Path(SOURCE_ARG).resolve()
SPEC = importlib.util.spec_from_file_location(
    'k1_fvm_case_util', SOURCE / 'tests/integration/fvm_case_e2e/fvm_case_util.py')
UTIL = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(UTIL)
INVALID_MODE = 'CHIPCAE_K1_STOP must be unset, legacy or ir'
TIMING = re.compile(r'\b(\w+)=([^ ]+)')
SECONDS = re.compile(r' \w+_seconds=[^ ]+')


def check(ok, message):
    if not ok:
        raise AssertionError(message)


def load(path):
    return json.loads(path.read_text(encoding='utf-8'))


def timings(run):
    result = {}
    for event in run['events']:
        if event['kind'] != 20 or ' assembly_seconds=' not in event['text']:
            continue
        field = event['text'].split()[0]
        key = (event['rank'], field)
        check(key not in result, f'duplicate timing line: {key}')
        result[key] = event['text']
    return result


def gate(run, ranks=1, thermal=False):
    lines = timings(run)
    fields = ('T', 'U') if thermal else ('U',)
    check(set(lines) == {(r, f) for r in range(ranks) for f in fields}, lines)
    for (rank, field), line in lines.items():
        if field == 'T':
            check('k1_stop=' not in line, f'T field changed: {line}')
            continue
        values = dict(TIMING.findall(line))
        for key, expected in dict(k1_stop='ir', gate_precision='binary64',
                                  residual_precision='double-double', gate_order='csr_row',
                                  gate='1.000e-12', atol='0').items():
            check(values.get(key) == expected, f'K1-E2/E4 missing {key}={expected}: {line}')
        for key in ('rel_d', 'rel_e', 'est_inf', 'est_abs', 'k1_ir_seconds'):
            check(key in values and math.isfinite(float(values[key]))
                  and float(values[key]) >= 0, f'invalid {key}: {line}')
        check(float(values['rel_d']) <= 1e-12, f'protected K1 gate: {line}')
        check('ir_corrections' in values and 0 <= int(values['ir_corrections']) <= 9, line)
        check(int(values['refine_iterations']) == int(values['ir_corrections']) + 1, line)
        factor = float(values['factor_seconds'])
        check(math.isfinite(factor) and factor > 0, line)
        print(f'observation (not expected) rank={rank} {line} '
              f'ir_factor_ratio={float(values["k1_ir_seconds"]) / factor:.9g} '
              f'abs_denominator_over_b={float(values["est_abs"]) / 2**-53:.9g}', flush=True)
    return lines


def analytic(run, linear):
    p, displacement = run['points'], run['u']
    x, y, z = p.T
    alpha = 1e-5
    if linear:
        # theta=T-T_ref=1000*z, with z measured from the bottom face.
        exact = alpha * np.column_stack((1000 * z * x, 1000 * z * y,
                                         500 * (z*z - x*x - y*y)))
    else:
        exact = alpha * 50 * p
    centered = p - np.mean(p, axis=0)
    modes = np.zeros((len(p), 3, 6))
    modes[:, :, :3] = np.eye(3)
    for axis in range(3):
        modes[:, :, 3 + axis] = np.cross(np.eye(3)[axis], centered)
    basis = modes.reshape((-1, 6))
    difference = (displacement - exact).ravel()
    coefficients, _, rank, _ = np.linalg.lstsq(basis, difference, rcond=None)
    check(rank == 6, 'rigid motion basis must have rank six')
    error = float(np.max(np.abs(difference - basis @ coefficients)))
    scale = float(np.max(np.abs(exact)))
    check(scale > 0 and error <= 1e-8 * scale,
          f'protected K1-E2/E3 analytic error={error:.17g}, bound={1e-8*scale:.17g}')
    if linear:
        pv = UTIL.summary(run['manifest'])['warpage_pv']
        check(abs(pv - 1.3e-3) <= 1e-8 * 1.3e-3, f'K1-E2 analytic PV: {pv:.17g}')
    print(f'analytic linear={linear} error={error:.17g} scale={scale:.17g}', flush=True)


def residual_bits(run):
    return [(e['solver'], e['field'], e['window'], e['iteration'],
             struct.pack('<d', e['norm']), e['flags'], e['code'], e['error_code'])
            for e in run['events'] if e['kind'] == 11]


def canonical(run):
    """Coordinate alignment with an explicit check of duplicated shared points."""
    p, u = run['points'], run['u']
    unique, inverse = np.unique(p, axis=0, return_inverse=True)
    selected = np.empty((len(unique), 3))
    scale = float(np.max(np.abs(u)))
    check(scale > 0, 'nonzero manufactured displacement required')
    for i in range(len(unique)):
        values = u[inverse == i]
        check(float(np.max(np.abs(values - values[0]))) <= 1e-8 * scale,
              f'inconsistent shared point {unique[i]}')
        selected[i] = values[0]
    return unique, selected


def contract(legacy, ir):
    a, b = legacy['manifest'], ir['manifest']
    # Fixture supplies field names/types only; no fixture values are numerical oracles.
    fixture = load(SOURCE / 'tests/contract/fixtures/i6-result-subset-1.0/'
                   'manifest_fem/fem.manifest.json')
    check(a['fields'] == b['fields'], 'K1-E6 fields changed')
    check(a['partitions'] == b['partitions'], 'K1-E6 partitions changed')
    check(len(a['summary']) == len(b['summary']), 'K1-E6 summary count changed')
    for old, new in zip(a['summary'], b['summary']):
        check(old['name'] == new['name'] and old['unit'] == new['unit'],
              'K1-E6 summary name/unit changed')
        check(('method' in old) == ('method' in new)
              and old.get('method') == new.get('method'),
              'K1-E6 summary method presence/value changed')
    tmax = [item for item in a['summary'] if item['name'] == 'T_max']
    check(len(tmax) == 1 and 'method' not in tmax[0],
          'K1-E6 legacy T_max must omit method')
    check(a['schema_version'] == b['schema_version'] == fixture['schema_version'],
          'I6 manifest schema')
    fixture_fields = {f['name']: f for f in fixture['fields']}
    required = {f['name'] for f in fixture['fields'] if not f.get('reserved', False)}
    check({f['name'] for f in a['fields']} >= required, 'missing I6 FEM fields')
    for field in a['fields']:
        if field['name'] in fixture_fields:
            reference = fixture_fields[field['name']]
            for key in ('components', 'dtype', 'location', 'unit'):
                check(field[key] == reference[key], f'K1-E6 fixture field shape {field}')
    check({f['name'] for f in a['fields']} >= {'T', 'U'}, 'missing FEM fields')
    check(set(a['partitions']) == set(fixture['partitions']), 'I6 partition shape')
    check(legacy['datasets'] == ir['datasets'], 'K1-E6 HDF5 dataset paths/shapes/dtypes changed')
    for run in (legacy, ir):
        check(run['u'].shape == run['points'].shape and run['u'].dtype == np.dtype('float64'),
              'I6 U must have three float64 components per point')
        analytic(run, linear=True)


class Runs:
    def __init__(self, root):
        self.root = root
        self.cache = {}

    def run(self, kind, kz, mode, ranks=1, expect_success=True):
        key = (kind, kz, mode, ranks)
        if key in self.cache:
            return self.cache[key]
        work = self.root / f'run{len(self.cache)}'
        work.mkdir()
        strip = UTIL.bimaterial_strip(work)
        stack = load(work / 'stack.json')
        check(len(stack['layers']) == 2, 'two equal layers required')
        for layer in stack['layers']:
            layer['n_elem_min'] = kz
        if kind == 'linear_cut':
            xmin, ymin, xmax, ymax = stack['outline']['box']
            stack['outline']['polygon'] = [[xmin, ymin], [xmax, ymin],
                                           [xmax, ymax], [xmin, ymax]]
        UTIL.write_json(work / 'stack.json', stack)
        linear = kind in ('linear', 'linear_cut')
        case = UTIL.thermal_case('fem') if linear else UTIL.solid_case('fem')
        case['mesh'] = strip['mesh']
        case['mesh'].update(nx=12, ny=4, levels=0, min_elems_per_layer=kz)
        case['solver']['linear']['preconditioner'] = 'direct'
        if linear:
            case['loads'] = [dict(type='fixed_temperature', target='face:zmin', T=300),
                             dict(type='fixed_temperature', target='face:zmax', T=400)]
        # Reuse the material builder, preserving the original mismatch only for E1/E4.
        cards = [UTIL.material(layer['material'], 2) for layer in stack['layers']]
        if kind == 'bimetal':
            cards[1]['thermal_expansion']['table']['columns']['alpha'] = [3e-5]
            cards[1]['conductivity'] = 4
        UTIL.write_json(work / 'materials.json',
                        dict(schema='chipcae.material_library/1', materials=cards))
        UTIL.write_json(work / 'case.json', case)
        UTIL.write_json(work / 'profile.json',
                        dict(schema='chipcae.run_profile/1.0', backend='cpu', ranks=ranks, gpus=0))
        env = os.environ.copy()
        # Each child is isolated from any caller-selected internal numerical switches.
        for name in ('CHIPCAE_K1_STOP', 'CHIPCAE_ACCURACY_Q2_EXPORT', 'CHIPCAE_Q2_ASSEMBLY'):
            env.pop(name, None)
        if mode is not None:
            env['CHIPCAE_K1_STOP'] = mode
        command = [EXE, '--case', 'case.json', '--profile', 'profile.json', '--out', 'out']
        if ranks > 1:
            check(bool(MPIEXEC) and Path(MPIEXEC).is_file(), 'K1-E5 requires an MPI launcher')
            command = [MPIEXEC, '-np', str(ranks), *command]
        start = time.monotonic()
        process = subprocess.run(command, cwd=work, env=env, capture_output=True,
                                 text=True, timeout=180)
        elapsed = time.monotonic() - start
        detail = f'{key!r} exit={process.returncode}\n{process.stdout}\n{process.stderr}'
        print(f'observation (not expected) {key!r} wall_seconds={elapsed:.9g}', flush=True)
        result = dict(process=process, work=work)
        self.cache[key] = result
        if not expect_success:
            return result
        check(process.returncode == 0, detail)
        out = work / 'out'
        manifest = load(out / 'manifest.json')
        counts = tuple(manifest['mesh'][name] for name in ('nx0', 'ny0', 'nz'))
        # K1.10 kz counts cells per layer: kz=2..16 means nz=4..32,
        # hence 3*(2*12+1)*(2*4+1)*(2*nz+1) = 6075..43875 Q2 DOFs.
        check(counts == (12, 4, 2 * kz), f'actual mesh differs from requested kz: {counts}')
        decoded = UTIL.decode_events((out / 'events.bin').read_bytes())
        check(decoded and decoded[-1]['kind'] == 19 and decoded[-1]['exit_code'] == 0,
              'missing successful run completion')
        check(len(manifest['steps']) == 1, 'one output step required')
        datasets = {}
        with h5py.File(out / manifest['steps'][0]['file'], 'r') as f:
            f.visititems(lambda name, obj: datasets.update({name: (obj.shape, obj.dtype.str)})
                         if isinstance(obj, h5py.Dataset) else None)
            points = f['VTKHDF/hex/Points'][:]
            displacement = f['VTKHDF/hex/PointData/U'][:]
        check(points.ndim == 2 and points.shape[1] == 3 and len(points) > 0, 'invalid point shape')
        check(displacement.shape == points.shape, 'invalid U shape')
        check(np.all(np.isfinite(points)) and np.all(np.isfinite(displacement)), 'nonfinite FEM output')
        result.update(manifest=manifest, events=decoded, points=points, u=displacement,
                      datasets=datasets)
        return result


def acceptance(root):
    runs = Runs(root)
    unset = runs.run('bimetal', 2, None)
    legacy = runs.run('bimetal', 2, 'legacy')
    check(unset['u'].dtype == legacy['u'].dtype and unset['u'].shape == legacy['u'].shape
          and unset['u'].tobytes() == legacy['u'].tobytes(), 'protected K1-E1 U changed')
    a, b = residual_bits(unset), residual_bits(legacy)
    check(a and {e[1] for e in a} == {'U'} and a == b, 'protected K1-E1 Residual changed')
    a, b = timings(unset), timings(legacy)
    check(set(a) == {(0, 'U')} and a.keys() == b.keys(), 'missing legacy U timing')
    for key in a:
        check('k1_stop=' not in a[key] and 'k1_stop=' not in b[key], 'legacy has K1 suffix')
        check(SECONDS.sub('', a[key]) == SECONDS.sub('', b[key]),
              f'protected K1-E1 timing changed: {a[key]}\n{b[key]}')
    invalid_failures = []
    for mode in ('IR', '', 'invalid'):
        invalid = runs.run('bimetal', 2, mode, expect_success=False)['process']
        if invalid.returncode != 1 or INVALID_MODE not in invalid.stderr:
            invalid_failures.append(
                f'K1-E1 invalid mode {mode!r}: expected exit=1 and {INVALID_MODE!r}; '
                f'got exit={invalid.returncode}, stderr={invalid.stderr!r}')
    check(not invalid_failures, '\n'.join(invalid_failures))
    print('PASS K1-E1 (including empty and unknown modes)', flush=True)

    linear2 = runs.run('linear', 2, 'ir')
    for run in (linear2, runs.run('linear', 8, 'ir')):
        gate(run, thermal=True)
        analytic(run, linear=True)
    print('PASS K1-E2', flush=True)

    uniform = runs.run('uniform', 4, 'ir')
    gate(uniform)
    analytic(uniform, linear=False)
    print('PASS K1-E3', flush=True)

    for kz in (2, 4, 8, 16):
        run = runs.run('bimetal', kz, 'ir')
        gate(run)
        # Legacy residuals are observations, not gate expectations.
        old = runs.run('bimetal', kz, 'legacy')
        for key, line in timings(old).items():
            print(f'observation (not expected) legacy kz={kz} {key}: {line}', flush=True)
    print('PASS K1-E4/E4b', flush=True)

    parallel = runs.run('uniform', 4, 'ir', ranks=2)
    gate(parallel, ranks=2)
    analytic(parallel, linear=False)
    p1, u1 = canonical(uniform)
    p2, u2 = canonical(parallel)
    check(np.array_equal(p1, p2), 'K1-E5 coordinate sets differ')
    relative = float(np.linalg.norm(u1 - u2) / np.linalg.norm(u1))
    check(math.isfinite(relative) and relative <= 1e-8, f'K1-E5 MPI U relative error={relative}')
    print('PASS K1-E5', flush=True)

    contract(runs.run('linear', 2, 'legacy'), linear2)
    print('PASS K1-E6', flush=True)

    cut_legacy = runs.run('linear_cut', 2, None)
    cut_ir = runs.run('linear_cut', 2, 'ir')
    a, b = timings(cut_legacy), timings(cut_ir)
    check(set(a) == set(b) == {(0, 'T'), (0, 'U')}, 'K1-E7 missing cut timing')
    for key in a:
        check('k1_stop=' not in a[key] and 'k1_stop=' not in b[key],
              f'K1-E7 cut has K1 suffix: {a[key]}\n{b[key]}')
    check(cut_legacy['u'].dtype == cut_ir['u'].dtype
          and cut_legacy['u'].shape == cut_ir['u'].shape
          and cut_legacy['u'].tobytes() == cut_ir['u'].tobytes(), 'K1-E7 cut U changed')
    a, b = residual_bits(cut_legacy), residual_bits(cut_ir)
    check(a and {e[1] for e in a} == {'T', 'U'} and a == b, 'K1-E7 cut Residual changed')
    print('PASS K1-E7', flush=True)


if __name__ == '__main__':
    try:
        with tempfile.TemporaryDirectory(prefix='k1-stop-') as directory:
            acceptance(Path(directory))
    except (AssertionError, OSError, subprocess.TimeoutExpired) as error:
        print(f'FAIL: {error}', file=sys.stderr)
        sys.exit(1)
