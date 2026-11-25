"""Synthetic CLI acceptance for column mapping and the independent cuboid MFD limit.

Catches swapped/local column indices, layer-average conductivity, missing diagnostics,
incorrect vertex reconstruction, and partition-dependent diagnostic bytes. The oracle
uses polygon clipping and half-cell resistances, never product mapping/solver helpers.
I1 1.2: baseline 750106f includes copper-kz-ez (#236), hence Voigt in all axes.
"""
import copy
import hashlib
import itertools
import json
import os
from pathlib import Path
import shlex
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

try:
    import h5py
    import numpy as np
except ImportError as error:
    print(f'SKIP: NumPy/h5py unavailable: {error}', flush=True)
    sys.exit(77)

RINGS = (((300, 400), (2700, 400), (2700, 1600), (300, 1600)),
         ((3500, 2300), (5200, 2300), (5200, 5900), (3500, 5900)),
         ((600, 3200), (2600, 3200), (600, 5600)))
FIELDS = ('T', 'U', 'layer_id', 'material_id', 'cell_kind', 'elem_gid', 'volume',
          'copper_fraction', 'conductivity_diag')
SHAPE = (6, 6, 5)
LAYER_TOPS = np.array([.0004, .0005, .0009])


def card(name, young, poisson, alpha, conductivity):
    def table(values):
        return dict(temperatures=[300], columns={k: [v] for k, v in values.items()},
                    interpolation='constant', extrapolation='clamp')
    return dict(id=name, schema_version='1', unit_system='SI', model='thermo_elastic',
                density=2000, conductivity=conductivity,
                elastic=dict(symmetry='isotropic', table=table(dict(E=young, nu=poisson))),
                thermal_expansion=dict(symmetry='isotropic', cte_type='tangent',
                                       stress_free_temperature=300, table=table(dict(alpha=alpha))),
                provenance=dict(source_type='illustrative', confidence='illustrative'))


def fixture(folder):
    """CCPOLY01 layout, as in tests/unit/copper_fraction/test_fraction.py."""
    name = b'sig'
    data = b'CCPOLY01' + struct.pack('<dII', 1e-6, 1, len(name)) + name
    offsets = [0]
    for ring in RINGS:
        offsets.append(offsets[-1] + len(ring))
    data += struct.pack('<I', len(RINGS)) + struct.pack('<4q', *offsets)
    data += b''.join(struct.pack('<qq', *point) for ring in RINGS for point in ring)
    (folder / 'copper.polyset').write_bytes(data)
    layers = [dict(name=name, kind='Explicit', material='fr4_eq', thickness=thickness,
                   n_elem_min=count, grading=1)
              for name, thickness, count in [('core1', .0004, 2), ('sig', .0001, 1),
                                             ('core2', .0004, 2)]]
    layers[1].update(kind='Patterned', copper=dict(metal='cu', polygons=dict(
        file='copper.polyset', layer='sig')))
    stack = dict(id='synthetic-trace', unit='m', outline=dict(box=[0, 0, .006, .006]),
                 symmetry='none', package_type='synthetic', layers=layers,
                 external_refs=[dict(path='copper.polyset', sha256=hashlib.sha256(data).hexdigest())])
    materials = dict(schema='chipcae.material_library/1', materials=[
        card('fr4_eq', 25e9, .3, 1e-5, .3), card('cu', 110e9, .34, 17e-6, 390)])
    for filename, value in [('stack.json', stack), ('materials.json', materials)]:
        (folder / filename).write_text(json.dumps(value), encoding='utf-8')
    return dict(schema='chipcae.case/1.0', stack=dict(ref='stack.json'),
                materials=dict(ref='materials.json'),
                mesh=dict(type='aligned_cartesian_cut', nx=6, ny=6, levels=1,
                          min_elems_per_layer=1, max_ratio=10),
                physics=[dict(id='thermal', solver='fvm.conduction', mode='steady'),
                         dict(id='mech', solver='fvm.solid', temp_from='thermal', T_ref=300)],
                loads=[dict(type='fixed_temperature', target='face:zmin', T=300),
                       dict(type='convection', target='face:zmax', h=50, T_inf=400),
                       dict(type='heat_source', target='layer:sig', q_vol=2e8)],
                constraints=dict(rigid='3-2-1'),
                solver=dict(linear=dict(preconditioner='jacobi', rtol=1e-14, max_iter=2000)),
                output=dict(format='vtkhdf', fields=list(FIELDS)))


def run_cli(folder, exe, mpiexec, ranks, fem=False):
    folder.mkdir()
    case = fixture(folder)
    if fem:
        case = copy.deepcopy(case)
        for physics in case['physics']:
            physics['solver'] = physics['solver'].replace('fvm.', 'fem.')
        case['solver']['linear']['preconditioner'] = 'direct'
        # The loader accepts explicit volume selection only for the FVM family.
        case['output']['fields'].remove('volume')
    (folder / 'case.json').write_text(json.dumps(case), encoding='utf-8')
    profile = dict(schema='chipcae.run_profile/1.0', ranks=ranks, backend='cpu', gpus=0)
    (folder / 'profile.json').write_text(json.dumps(profile), encoding='utf-8')
    command = [exe, '--case', str(folder / 'case.json'), '--profile',
               str(folder / 'profile.json'), '--out', str(folder / 'out')]
    if ranks > 1:
        command = [mpiexec, '-np', str(ranks), *command]
    print('COMMAND', shlex.join(command), flush=True)
    result = subprocess.run(command, cwd=folder, capture_output=True, text=True, timeout=240,
                            env=dict(os.environ, OMP_NUM_THREADS='2', OPENBLAS_NUM_THREADS='1'))
    assert result.returncode == 0, (
        f'CLI exit {result.returncode}\nstderr (last 20 lines):\n' +
        '\n'.join(result.stderr.splitlines()[-20:]))
    out = folder / 'out'
    return out, json.loads((out / 'manifest.json').read_text(encoding='utf-8'))


def maximum(manifest, fvm=True):
    entries = [entry for entry in manifest['summary'] if entry['name'] == 'T_max']
    assert len(entries) == 1, f'expected unique T_max, got {entries}'
    if fvm:
        assert entries[0]['method'] == 'FVM cell centers', entries[0]
    value = float(entries[0]['value'])
    assert np.isfinite(value), entries[0]
    return value


def read_hex(out, manifest):
    """Connectivity and offset arrays restart at each partition, points are concatenated."""
    assert len(manifest['steps']) == 1, 'expected one steady step'
    with h5py.File(out / manifest['steps'][0]['file'], 'r') as file:
        group = file['VTKHDF/hex']
        points = group['Points'][:]
        point = {key: value[:] for key, value in group['PointData'].items()}
        cell = {key: value[:] for key, value in group['CellData'].items()}
        counts = [group[key][:] for key in
                  ('NumberOfPoints', 'NumberOfCells', 'NumberOfConnectivityIds')]
        assert len({len(count) for count in counts}) == 1
        connectivity, offsets, types = (group[key][:] for key in ('Connectivity', 'Offsets', 'Types'))
    npart, ncell, nconn = (int(np.sum(count)) for count in counts)
    assert points.shape == (npart, 3)
    assert len(types) == ncell and np.all(types == 12), 'only VTK hexes expected'
    assert len(connectivity) == nconn
    assert len(offsets) == ncell + len(counts[0])
    nodes = []
    po = co = io = oo = 0
    for pn, cn, ni in zip(*counts):
        pn, cn, ni = int(pn), int(cn), int(ni)
        local_offsets = offsets[oo:oo + cn + 1]
        assert local_offsets[0] == 0 and local_offsets[-1] == ni
        assert np.all(np.diff(local_offsets) == 8), 'eight vertices per hex'
        for e in range(cn):
            local = connectivity[io + local_offsets[e]:io + local_offsets[e + 1]]
            assert len(set(map(int, local))) == 8 and np.all((0 <= local) & (local < pn))
            nodes.append(po + local)
        po, co, io, oo = po + pn, co + cn, io + ni, oo + cn + 1
    assert (po, co, io, oo) == (npart, ncell, nconn, len(offsets))
    assert ncell == 180, f'expected 180 cells, got {ncell}'
    for key in FIELDS:
        data = point[key] if key in ('T', 'U') else cell[key]
        shape = ((npart, 3) if key == 'U' else (npart,)) if key in ('T', 'U') else (
            (ncell, 3) if key == 'conductivity_diag' else (ncell,))
        assert data.shape == shape, (key, data.shape, shape)
        assert np.all(np.isfinite(data)), f'nonfinite {key}'
    assert np.all(np.isfinite(points)) and np.all(cell['volume'] > 0)
    for key in ('copper_fraction', 'conductivity_diag'):
        assert cell[key].dtype == np.dtype('float64'), (key, cell[key].dtype)
    gids = cell['elem_gid']
    assert np.issubdtype(gids.dtype, np.integer)
    assert len(set(map(int, gids))) == ncell, 'duplicate elem_gid'
    order = np.argsort(gids)
    nodes = np.asarray(nodes)[order]
    cell = {key: value[order] for key, value in cell.items()}
    vertices = points[nodes]
    lower, upper = vertices.min(axis=1), vertices.max(axis=1)
    widths = upper - lower
    assert np.all(widths > 0)
    axes = [np.unique(np.r_[lower[:, d], upper[:, d]]) for d in range(3)]
    assert tuple(len(axis) - 1 for axis in axes) == SHAPE
    for axis, expected in zip(axes, (np.linspace(0, .006, 7), np.linspace(0, .006, 7),
                                    np.array([0, .0002, .0004, .0005, .0007, .0009]))):
        assert np.max(np.abs(axis - expected)) <= 1e-15, (axis, expected)
    keys = []
    for lo, hi, corners in zip(lower, upper, vertices):
        expected = set(itertools.product(*zip(lo, hi)))
        assert set(map(tuple, corners)) == expected, 'hex is not an axis-aligned cuboid'
        key = tuple(int(np.searchsorted(axis, x)) for axis, x in zip(axes, lo))
        assert all(abs(hi[d] - axes[d][key[d] + 1]) <= 1e-15 for d in range(3))
        keys.append(key)
    assert len(set(keys)) == ncell, 'duplicate geometry key'
    assert set(keys) == set(itertools.product(*(range(n) for n in SHAPE))), 'missing geometry key'
    volume = np.prod(widths, axis=1)
    assert np.max(relative(cell['volume'], volume)) <= 1e-12
    layers = np.searchsorted(LAYER_TOPS, ((lower + upper) / 2)[:, 2])
    return dict(points=points, point=point, cell=cell, nodes=nodes, lower=lower, upper=upper,
                widths=widths, keys=keys, volume=volume, sig=layers == 1, axes=axes)


def clip_area(ring, lo, hi):
    polygon = [(x * 1e-6, y * 1e-6) for x, y in ring]
    for dimension, bound, sign in ((0, lo[0], 1), (0, hi[0], -1),
                                   (1, lo[1], 1), (1, hi[1], -1)):
        clipped = []
        if not polygon:
            return 0.
        previous = polygon[-1]
        for current in polygon:
            inside = sign * (current[dimension] - bound) >= 0
            was_inside = sign * (previous[dimension] - bound) >= 0
            if inside != was_inside:
                t = (bound - previous[dimension]) / (current[dimension] - previous[dimension])
                intersection = [previous[d] + t * (current[d] - previous[d]) for d in range(2)]
                intersection[dimension] = bound
                clipped.append(tuple(intersection))
            if inside:
                clipped.append(current)
            previous = current
        polygon = clipped
    return abs(sum(a[0] * b[1] - a[1] * b[0]
                   for a, b in zip(polygon, polygon[1:] + polygon[:1]))) / 2


def relative(actual, expected):
    actual, expected = np.asarray(actual), np.asarray(expected)
    return np.abs(actual - expected) / np.maximum(np.maximum(np.abs(actual), np.abs(expected)), 1e-30)


def reference(mesh, average=False):
    """Full width h: R_half=h/(2*k*A); internal faces add their two half resistances."""
    lo, hi, widths, sig = (mesh[key] for key in ('lower', 'upper', 'widths', 'sig'))
    area = widths[:, 0] * widths[:, 1]
    phi = np.zeros(180)
    for c in np.flatnonzero(sig):
        phi[c] = 19 / 60 if average else sum(clip_area(ring, lo[c], hi[c]) for ring in RINGS) / area[c]
    conductivity = np.repeat((.3 + 389.7 * phi)[:, None], 3, axis=1)
    matrix, rhs = np.zeros((180, 180)), np.where(sig, 2e8 * mesh['volume'], 0.)
    source = rhs.copy()
    by_key = {key: c for c, key in enumerate(mesh['keys'])}
    boundary = []
    for key, c in by_key.items():
        for d in range(3):
            face_area = mesh['volume'][c] / widths[c, d]
            gc = 2 * conductivity[c, d] * face_area / widths[c, d]
            neighbor = list(key)
            neighbor[d] += 1
            if neighbor[d] < SHAPE[d]:
                n = by_key[tuple(neighbor)]
                gn = 2 * conductivity[n, d] * face_area / widths[n, d]
                g = 1 / (1 / gc + 1 / gn)
                matrix[c, c] += g
                matrix[n, n] += g
                matrix[c, n] -= g
                matrix[n, c] -= g
            if d == 2:
                if key[d] == 0:
                    boundary.append((c, gc, 300.))
                if key[d] == SHAPE[d] - 1:
                    boundary.append((c, 1 / (1 / gc + 1 / (50 * face_area)), 400.))
    for c, g, temperature in boundary:
        matrix[c, c] += g
        rhs[c] += g * temperature
    temperature = np.linalg.solve(matrix, rhs)
    powers = np.r_[source, [g * (t - temperature[c]) for c, g, t in boundary]]
    heat_in = float(np.sum(np.maximum(powers, 0)))
    heat_out = float(np.sum(np.maximum(-powers, 0)))
    return dict(phi=phi, conductivity=conductivity, temperature=temperature,
                heat_in=heat_in, heat_out=heat_out)


def energy_record(path):
    data, offset, records = path.read_bytes(), 0, []
    while offset < len(data):
        assert len(data) - offset >= 16 and data[offset:offset + 8] == b'CAELOG1\0'
        length, crc = struct.unpack_from('<II', data, offset + 8)
        body = data[offset + 16:offset + 16 + length]
        assert len(body) == length and length >= 17 and zlib.crc32(body) == crc
        _, _, kind = struct.unpack_from('<QqB', body)
        if kind == 20:
            payload = body[17:]
            assert len(payload) >= 12
            level, rank, size = struct.unpack_from('<iiI', payload)
            assert size <= len(payload) - 12
            message = payload[12:12 + size].decode('utf-8')
            position = 12 + size
            assert len(payload) >= position + 4
            code_size, = struct.unpack_from('<I', payload, position)
            position += 4
            assert len(payload) == position + code_size + 4
            payload[position:position + code_size].decode('utf-8')
            struct.unpack_from('<I', payload, position + code_size)
            if message.startswith('FVM_ENERGY '):
                records.append(json.loads(message[len('FVM_ENERGY '):]))
        offset += 16 + length
    assert len(records) == 1, f'expected one FVM_ENERGY record, got {records}'
    record = records[0]
    assert {'heat_in', 'heat_out', 'imbalance', 'iterations'} <= record.keys()
    assert all(np.isfinite(record[key]) and record[key] >= 0 for key in record)
    assert isinstance(record['iterations'], int) and 0 < record['iterations'] <= 2000
    return record


def internal_vertex_error(mesh, temperature):
    adjacency = {}
    for c, nodes in enumerate(mesh['nodes']):
        for node in nodes:
            key = tuple(mesh['points'][node])
            adjacency.setdefault(key, set()).add(c)
    internal = {tuple(p) for p in itertools.product(*(axis[1:-1] for axis in mesh['axes']))}
    assert len(internal) == 100 and internal <= adjacency.keys()
    expected = {}
    for key in sorted(internal):
        cells = sorted(adjacency[key])
        assert len(cells) == 8, (key, cells)
        expected[key] = np.average(temperature[cells], weights=mesh['volume'][cells])
    errors = [abs(float(t) - expected[tuple(p)])
              for p, t in zip(mesh['points'], mesh['point']['T']) if tuple(p) in internal]
    assert len(errors) >= 100, 'check every partition copy of each internal vertex'
    return max(errors)


class FvmTraceMapAcceptance(unittest.TestCase):
    def test_column_mapping_against_independent_discrete_reference(self):
        exe, mpiexec = sys.argv[1:]
        runs = {}
        with tempfile.TemporaryDirectory(prefix='fvm-trace-map-') as directory:
            root = Path(directory)
            for ranks in (1, 2):
                with self.subTest(ranks=ranks):
                    out, manifest = run_cli(root / f'fvm-np{ranks}', exe, mpiexec, ranks)
                    mesh = read_hex(out, manifest)
                    ref, averaged = reference(mesh), reference(mesh, average=True)
                    sig, cell = mesh['sig'], mesh['cell']
                    phi, k = cell['copper_fraction'], cell['conductivity_diag']
                    phi_error = float(np.max(np.abs(phi[sig] - ref['phi'][sig])))
                    k_error = float(np.max(relative(k, ref['conductivity'])))
                    area_error = abs(float(np.sum(phi[sig] * np.prod(mesh['widths'][sig, :2], axis=1))) - 1.14e-5)
                    tmax = maximum(manifest)
                    reference_max = float(np.max(ref['temperature']))
                    t_error = abs(tmax - reference_max)
                    vertex_error = internal_vertex_error(mesh, ref['temperature'])
                    energy = energy_record(out / 'events.bin')
                    energy_errors = {key: float(relative(energy[key], ref[key])) for key in ('heat_in', 'heat_out')}
                    discrimination = abs(reference_max - float(np.max(averaged['temperature'])))
                    table = np.empty((6, 6))
                    for c, (i, j, _) in enumerate(mesh['keys']):
                        if sig[c]:
                            table[j, i] = phi[c]
                    print(f'np={ranks} phi table (rows y, columns x):\n{table}', flush=True)
                    print(f'np={ranks} phi_abs={phi_error:.17g} k_rel={k_error:.17g} '
                          f'area_abs_m2={area_error:.17g}\nT_max={tmax:.17g} '
                          f'reference={reference_max:.17g} T_abs_K={t_error:.17g} '
                          f'vertex_abs_K={vertex_error:.17g} layer_average_delta_K={discrimination:.17g}\n'
                          f'energy={energy} reference_heat_in={ref["heat_in"]:.17g} '
                          f'reference_heat_out={ref["heat_out"]:.17g} energy_relative_errors={energy_errors}', flush=True)
                    runs[ranks] = (tmax, mesh)
                    # Subtests retain every numerical failure and still exercise np=2 and FEM.
                    with self.subTest(check='fractions and conservation'):
                        self.assertLessEqual(phi_error, 1e-12)
                        self.assertTrue(np.all(phi[~sig] == 0), 'non-copper fractions exactly zero')
                        partial = sorted(phi[sig][(phi[sig] > 1e-12) & (phi[sig] < 1 - 1e-12)])
                        distinct = []
                        for value in partial:
                            if not distinct or value - distinct[-1] > 1e-12:
                                distinct.append(value)
                        self.assertGreaterEqual(len(distinct), 4)
                        self.assertTrue(np.any(np.abs(phi[sig]) <= 1e-12))
                        self.assertTrue(np.any(np.abs(phi[sig] - 1) <= 1e-12))
                        self.assertLessEqual(area_error, 1e-12 * 3.6e-5)
                    with self.subTest(check='conductivity and endpoints'):
                        self.assertLessEqual(k_error, 1e-12)
                        for fraction, endpoint in ((0, .3), (1, 390)):
                            selected = sig & (np.abs(ref['phi'] - fraction) <= 1e-12)
                            self.assertTrue(np.any(selected))
                            self.assertLessEqual(float(np.max(relative(k[selected], endpoint))), 1e-12)
                    with self.subTest(check='temperature and discrimination'):
                        self.assertGreaterEqual(discrimination, .1)
                        self.assertLessEqual(t_error, 1e-9)
                        self.assertLessEqual(vertex_error, 1e-9)
                    with self.subTest(check='energy'):
                        for error in energy_errors.values():
                            self.assertLessEqual(error, 1e-9)
                        self.assertLessEqual(energy['imbalance'], 1e-9)
            if len(runs) == 2:
                with self.subTest(check='MPI invariance'):
                    serial_t, serial = runs[1]
                    parallel_t, parallel = runs[2]
                    difference = float(relative(serial_t, parallel_t))
                    print(f'np=2 versus np=1 T_max relative difference={difference:.17g}', flush=True)
                    self.assertLessEqual(difference, 1e-9)
                    np.testing.assert_array_equal(serial['cell']['elem_gid'], parallel['cell']['elem_gid'])
                    for key in ('copper_fraction', 'conductivity_diag'):
                        a, b = serial['cell'][key], parallel['cell'][key]
                        self.assertEqual(a.shape, b.shape)
                        self.assertEqual(a.tobytes(), b.tobytes(), f'{key}: float64 bytes differ after GID alignment')
                        print(f'np=2 versus np=1 {key}: float64 bytes identical', flush=True)
            with self.subTest(check='FEM informational comparison'):
                _, manifest = run_cli(root / 'fem-np1', exe, mpiexec, 1, fem=True)
                fem_t = maximum(manifest, fvm=False)
                difference = float(relative(fem_t, runs[1][0])) if 1 in runs else float('nan')
                print(f'FEM T_max={fem_t:.17g} relative difference from FVM={difference:.17g} (informational)', flush=True)


if __name__ == '__main__':
    unittest.main(argv=[sys.argv[0]], verbosity=2)
