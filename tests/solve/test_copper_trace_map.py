"""Synthetic public-CLI acceptance for global-GID copper trace mapping.

The Q2 oracle below is a standalone NumPy assembly, with no solver/mesh helpers.
Illustrative material values are not supplier data. Cut phi/s correction belongs
in M2; the cut checks here lock initialized diagnostics and parent-GID association.
"""
import copy
import hashlib
import itertools
import json
import os
from pathlib import Path
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

import h5py
import numpy as np

# tests/integration/f1130_uncut_cli/test_uncut_cli_e2e.py:26, frozen acceptance tolerance.
PARALLEL_RTOL = 1e-8
DIAGNOSTICS = ('copper_fraction', 'conductivity_diag')
LEGACY_FIELDS = ('T', 'U', 'layer_id', 'material_id', 'cell_kind', 'elem_gid')
EXE, SOURCE = sys.argv[1:3]
WIDTH = .04
FIXED, AMBIENT, H = 300., 400., 20.


def conductivity(f, base=.3):
    return np.array([f * 390 + (1 - f) * base] * 2 +
                    [f * 390 + (1 - f) * base])


def card(name, E, nu, alpha, k):
    def table(columns):
        return dict(temperatures=[300], columns={key: [v] for key, v in columns.items()},
                    interpolation='constant', extrapolation='clamp')
    return dict(id=name, schema_version='1', unit_system='SI', model='thermo_elastic',
                density=2000, conductivity=k,
                elastic=dict(symmetry='isotropic', table=table(dict(E=E, nu=nu))),
                thermal_expansion=dict(symmetry='isotropic', cte_type='tangent',
                                       stress_free_temperature=300, table=table(dict(alpha=alpha))),
                provenance=dict(source_type='illustrative', confidence='illustrative'))


def morton(xy):
    x, y = xy
    return sum(((x >> b) & 1) << (2*b) | ((y >> b) & 1) << (2*b+1) for b in range(32))


def columns(nx, ny):
    return sorted(itertools.product(range(nx), range(ny)), key=morton)


def q2_reference(axes, coefficients):
    """27-node Lagrange hexes; 3^3 volume and 3^2 Robin Gauss points."""
    fine = [np.sort(np.r_[a, (a[:-1] + a[1:]) / 2]) for a in axes]
    shape = tuple(len(a) for a in fine)
    points = np.array(list(itertools.product(*fine)))
    matrix = np.zeros((len(points), len(points)))
    rhs = np.zeros(len(points))
    gauss, weights = np.polynomial.legendre.leggauss(3)
    gauss, weights = (gauss + 1) / 2, weights / 2
    local = np.array(list(itertools.product(range(3), repeat=3)))

    def basis(t):
        return np.array([(2*t-1)*(t-1), 4*t*(1-t), t*(2*t-1)]), np.array([4*t-3, 4-8*t, 4*t-1])

    for cell in itertools.product(*(range(len(a)-1) for a in axes)):
        delta = np.array([a[i+1]-a[i] for a, i in zip(axes, cell)])
        nodes = np.ravel_multi_index((2*np.array(cell) + local).T, shape)
        stiffness = np.zeros((27, 27))
        load = np.zeros(27)
        for qi in itertools.product(range(3), repeat=3):
            pairs = [basis(gauss[q]) for q in qi]
            gradient = np.empty((27, 3))
            for d in range(3):
                gradient[:, d] = np.prod([pairs[j][int(j == d)][local[:, j]] for j in range(3)], axis=0) / delta[d]
            weight = np.prod(weights[list(qi)]) * np.prod(delta)
            stiffness += (gradient * coefficients[cell]) @ gradient.T * weight
        if cell[2] == len(axes[2])-2:
            for qx, qy in itertools.product(range(3), repeat=2):
                bx, _ = basis(gauss[qx])
                by, _ = basis(gauss[qy])
                bz, _ = basis(1.)
                N = bx[local[:, 0]] * by[local[:, 1]] * bz[local[:, 2]]
                weight = weights[qx] * weights[qy] * delta[0] * delta[1]
                stiffness += H * np.outer(N, N) * weight
                load += H * AMBIENT * N * weight
        matrix[np.ix_(nodes, nodes)] += stiffness
        rhs[nodes] += load
    fixed = np.flatnonzero(np.abs(points[:, 2]) < 1e-14)
    free = np.setdiff1d(np.arange(len(points)), fixed)
    temperature = np.full(len(points), FIXED)
    temperature[free] = np.linalg.solve(matrix[np.ix_(free, free)],
                                        rhs[free] - matrix[np.ix_(free, fixed)] @ temperature[fixed])
    residual = matrix @ temperature - rhs
    assert np.max(np.abs(residual[free])) <= 1e-9, 'Q2 reference residual'
    return {tuple(np.round(p, 12)): float(t) for p, t in zip(points, temperature)}


class TraceMapTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='copper-trace-')
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.case = dict(
            schema='chipcae.case/1.0', stack=dict(ref='inputs/stack.json'),
            materials=dict(ref='materials.json'),
            mesh=dict(type='aligned_cartesian_cut', nx=2, ny=2, levels=1,
                      min_elems_per_layer=2, max_ratio=100),
            physics=[dict(id='thermal', solver='fem.conduction', mode='steady'),
                     dict(id='mech', solver='fem.solid', temp_from='thermal', T_ref=300)],
            loads=[dict(type='fixed_temperature', target='face:zmin', T=FIXED),
                   dict(type='convection', target='face:zmax', h=H, T_inf=AMBIENT)],
            constraints=dict(rigid='3-2-1'),
            solver=dict(linear=dict(preconditioner='direct', rtol=1e-13)),
            output=dict(format='vtkhdf'))
        self.stack = dict(id='trace_map', unit='m', outline=dict(box=[0, 0, WIDTH, WIDTH]),
                          symmetry='none', package_type='synthetic', layers=[
                              dict(name='trace', kind='Patterned', material='fr4_eq',
                                   thickness=.02, n_elem_min=2, grading=1,
                                   copper=dict(metal='cu', fraction=.3)),
                              dict(name='plain', kind='Explicit', material='fr4_eq',
                                   thickness=.02, n_elem_min=2, grading=1)])
        self.materials = [card('fr4_eq', 25e9, .3, 1e-5, .3),
                          card('cu', 110e9, .34, 17e-6, 390)]

    def polygons(self, stack, fractions, nx=2, ny=2):
        """Exact integer-coordinate stripes, including overlapping union rings."""
        result = copy.deepcopy(stack)
        rings = []
        for x, y in itertools.product(range(nx), range(ny)):
            f = float(fractions[x, y])
            if f <= 0:
                continue
            x0, y0 = x * WIDTH/nx, y * WIDTH/ny
            x1, y1 = x0 + f*WIDTH/nx, y0 + WIDTH/ny
            ring = [(round(a/1e-6), round(b/1e-6)) for a, b in
                    [(x0, y0), (x1, y0), (x1, y1), (x0, y1)]]
            rings.append(ring)
        # A duplicate ring must be unioned, not double-counted in conservation.
        if rings:
            rings.append(rings[0])
        name = b'trace'
        binary = b'CCPOLY01' + struct.pack('<dII', 1e-6, 1, len(name)) + name
        binary += struct.pack('<I', len(rings))
        offsets = np.cumsum([0] + [len(ring) for ring in rings])
        binary += b''.join(struct.pack('<q', int(i)) for i in offsets)
        binary += b''.join(struct.pack('<qq', *point) for ring in rings for point in ring)
        result['layers'][0]['copper'] = dict(metal='cu', polygons=dict(file='trace.polyset', layer='trace'))
        result['external_refs'] = [dict(path='trace.polyset', sha256=hashlib.sha256(binary).hexdigest())]
        return result, binary

    def run_case(self, name, stack=None, case=None, ranks=1, polyset=None, fallback=False,
                 expected=0, diagnostic=''):
        folder = self.root / name
        (folder/'inputs').mkdir(parents=True)
        (folder/'case.json').write_text(json.dumps(case or self.case))
        (folder/'inputs/stack.json').write_text(json.dumps(stack or self.stack))
        (folder/'materials.json').write_text(json.dumps(dict(schema='chipcae.material_library/1', materials=self.materials)))
        (folder/'profile.json').write_text(json.dumps(dict(schema='chipcae.run_profile/1.0', ranks=ranks, backend='cpu', gpus=0)))
        if polyset is not None:
            sidecars = {'trace.polyset': polyset} if isinstance(polyset, bytes) else polyset
            for filename, data in sidecars.items():
                (folder/'inputs'/filename).write_bytes(data)
        command = [EXE, '--case', str(folder/'case.json'), '--profile', str(folder/'profile.json'),
                   '--out', str(folder/'run')]
        if fallback:
            command += ['--cut-output', 'hex']
        if ranks > 1:
            launcher = os.environ.get('MPIEXEC_EXECUTABLE') or shutil.which('mpiexec') or shutil.which('mpirun')
            self.assertIsNotNone(launcher, 'MPI launcher required for ranks=2 acceptance')
            command = [launcher, '-np', str(ranks), *command]
        env = dict(os.environ, OMP_NUM_THREADS='1', OPENBLAS_NUM_THREADS='1')
        print('COMMAND', shlex.join(command), flush=True)
        result = subprocess.run(command, cwd=folder, env=env, capture_output=True, text=True, timeout=120)
        self.assertEqual(result.returncode, expected,
                         f'{name}: expected exit {expected}, got {result.returncode}\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}')
        self.assertIn(diagnostic, result.stderr)
        if expected:
            return None
        manifest = json.loads((folder/'run/manifest.json').read_text())
        blocks = {}
        with h5py.File(folder/'run'/manifest['steps'][0]['file']) as file:
            for block in ('hex', 'cut', 'eb_surface'):
                g = file['VTKHDF/'+block]
                data = dict(points=g['Points'][:], point={k: v[:] for k, v in g['PointData'].items()},
                            cell={k: v[:] for k, v in g['CellData'].items()})
                if block == 'hex':
                    conn = g['Connectivity'][:].reshape(-1, 8)
                    centers, node_offset, cell_offset = [], 0, 0
                    for npart, ncell in zip(g['NumberOfPoints'][:], g['NumberOfCells'][:]):
                        for e in range(int(ncell)):
                            centers.append(data['points'][node_offset+conn[cell_offset+e]].mean(axis=0))
                        node_offset += int(npart)
                        cell_offset += int(ncell)
                    data['centers'] = np.array(centers)
                blocks[block] = data
        return manifest, blocks

    def temperatures(self, result):
        data = result[1]['hex']
        out = {}
        for p, t in zip(data['points'], data['point']['T']):
            key = tuple(np.round(p, 12))
            if key in out:
                self.assertLessEqual(abs(out[key]-t), 1e-10, 'duplicate MPI vertex disagrees')
            out[key] = float(t)
        return out

    def compare_temperatures(self, a, b, atol, rtol=0):
        self.assertEqual(a.keys(), b.keys(), 'compare vertices by coordinates')
        aa, bb = np.array([a[p] for p in sorted(a)]), np.array([b[p] for p in sorted(a)])
        np.testing.assert_allclose(aa, bb, atol=atol, rtol=rtol)

    def one_dimensional(self, result, f=.3, base=.3):
        t = self.temperatures(result)
        kz = conductivity(f, base)[2]
        resistance = .02/kz + .02/.3
        flux = (AMBIENT-FIXED)/(resistance+1/H)
        expected = {p: FIXED+flux*(min(p[2], .02)/kz+max(p[2]-.02, 0)/.3) for p in t}
        print('1D max temperature error K=', max(abs(t[p]-expected[p]) for p in t), flush=True)
        self.compare_temperatures(t, expected, 1e-8)
        return expected

    def diagnostics(self, result, fractions, nx=2, ny=2, nz=4, trace_nz=2, base=.3, cut=False):
        manifest, blocks = result
        fields = {f['name']: f for f in manifest['fields']}
        for name, count in zip(DIAGNOSTICS, (1, 3)):
            self.assertIn(name, fields, f'missing manifest field {name}')
            self.assertEqual({key: fields[name][key] for key in ('location', 'components', 'dtype', 'unit')},
                             dict(location='cell', components=count, dtype='float64', unit=''))
        xy = columns(nx, ny)
        by_gid = {}
        for block in ('hex', 'cut', 'eb_surface'):
            data = blocks[block]
            cells = data['cell']
            gids = cells.get('elem_gid', np.empty(0, dtype=int))
            if not len(gids):
                continue
            for name, components in zip(DIAGNOSTICS, (1, 3)):
                self.assertIn(name, cells, f'{block}: missing {name}')
                self.assertEqual(cells[name].shape, (len(gids),) if components == 1 else (len(gids), 3))
                self.assertEqual(cells[name].dtype, np.dtype('float64'))
                self.assertTrue(np.all(np.isfinite(cells[name])))
            for e, gid in enumerate(gids):
                col, k = divmod(int(gid), nz)
                self.assertLess(col, len(xy), 'GID outside global column table')
                x, y = xy[col]
                f = float(fractions[x, y]) if k < trace_nz else 0.
                actual = cells['copper_fraction'][e]
                self.assertGreaterEqual(actual, 0)
                self.assertLessEqual(actual, 1)
                self.assertLessEqual(abs(actual-f), 1e-12, f'{block} gid={gid}')
                if k >= trace_nz:
                    self.assertEqual(float(actual), 0., 'non-copper fraction must be exactly zero')
                expected_k = conductivity(f, base) if k < trace_nz else np.full(3, .3)
                actual_k = cells['conductivity_diag'][e]
                relative = np.abs(actual_k-expected_k)/np.maximum.reduce([np.abs(actual_k), np.abs(expected_k), np.full(3, 1e-30)])
                self.assertLessEqual(float(relative.max()), 1e-14, f'{block} gid={gid}: {actual_k} vs {expected_k}')
                self.assertTrue(np.all(actual_k > 0), 'f=0 retains positive base conductivity')
                if block != 'eb_surface':
                    self.assertNotIn(int(gid), by_gid, 'cell exported twice')
                    by_gid[int(gid)] = (actual, actual_k)
                if block == 'hex' and not cut:
                    center = data['centers'][e]
                    self.assertAlmostEqual(center[0], (x+.5)*WIDTH/nx, delta=1e-12)
                    self.assertAlmostEqual(center[1], (y+.5)*WIDTH/ny, delta=1e-12)
            if block == 'hex':
                np.testing.assert_array_equal(cells['layer_id'], 1+(gids % nz >= trace_nz))
        if not cut:
            self.assertEqual(set(by_gid), set(range(nx*ny*nz)))
            # Count each xy column once even with multiple z cells in the copper layer.
            area = sum(float(by_gid[col*nz][0])*WIDTH**2/(nx*ny) for col in range(nx*ny))
            expected_area = float(np.sum(fractions))*WIDTH**2/(nx*ny)
            self.assertLessEqual(abs(area-expected_area)/max(abs(expected_area), 1e-30), 1e-12)
        return by_gid

    def test_uniform_stripes_fraction_polygons_and_one_dimensional(self):
        fraction = self.run_case('fraction')
        self.one_dimensional(fraction)
        stack, binary = self.polygons(self.stack, np.full((2, 2), .3))
        polygons = self.run_case('polygons-only', stack, polyset=binary)
        self.one_dimensional(polygons)
        self.compare_temperatures(self.temperatures(fraction), self.temperatures(polygons), 1e-10)
        self.diagnostics(fraction, np.full((2, 2), .3))
        self.diagnostics(polygons, np.full((2, 2), .3))

    def test_copper_base_counterexample_uses_fresh_inputs(self):
        # Sparse copper keeps the base-material contrast above 1 K with parallel kz.
        fraction = .01
        self.stack['layers'][0]['copper']['fraction'] = fraction
        reference_run = self.run_case('fr4-reference')
        reference = self.one_dimensional(reference_run, f=fraction)
        changed = copy.deepcopy(self.stack)
        changed['layers'][0]['material'] = 'cu'
        counter = self.run_case('copper-base', changed)
        self.one_dimensional(counter, f=fraction, base=390)
        actual = self.temperatures(counter)
        self.assertEqual(actual.keys(), reference.keys())
        difference = max(abs(actual[p]-reference[p]) for p in actual)
        print('copper-base deviation from fixed fr4 reference K=', difference, flush=True)
        self.assertGreater(difference, 1.)

    def check_q2_reference(self):
        axes = [np.linspace(0, WIDTH, 3), np.linspace(0, WIDTH, 3), np.array([0., .01, .02, .03, .04])]
        for ks in ([.3]*4, [.3, .3, 2., 2.]):
            with self.subTest(kz=ks):
                coefficients = {cell: np.full(3, ks[cell[2]]) for cell in itertools.product(range(2), range(2), range(4))}
                reference = q2_reference(axes, coefficients)
                resistance = sum(.01/k for k in ks)
                flux = (AMBIENT-FIXED)/(resistance+1/H)
                for p, actual in reference.items():
                    r = sum(max(0., min(.01, p[2]-.01*k))/value for k, value in enumerate(ks))
                    self.assertAlmostEqual(actual, FIXED+flux*r, delta=1e-8)

    def test_q2_reference_self_checks_homogeneous_and_layered(self):
        self.check_q2_reference()

    def test_single_copper_column_against_independent_q2(self):
        self.check_q2_reference()
        fractions = np.zeros((2, 2))
        fractions[1, 1] = .3  # Last global Morton column, owned by rank 1 in np=2.
        stack = copy.deepcopy(self.stack)
        stack['layers'] = stack['layers'][:1]
        stack['layers'][0].update(n_elem_min=1, thickness=.04)
        stack, binary = self.polygons(stack, fractions)
        case = copy.deepcopy(self.case)
        case['mesh']['min_elems_per_layer'] = 1
        axes = [np.linspace(0, WIDTH, 3), np.linspace(0, WIDTH, 3), np.array([0., .04])]
        coefficients = {(x, y, 0): conductivity(fractions[x, y]) for x, y in itertools.product(range(2), repeat=2)}
        reference = q2_reference(axes, coefficients)
        results = []
        for ranks in (1, 2):
            with self.subTest(ranks=ranks):
                result = self.run_case(f'single-column-{ranks}', stack, case, ranks, binary)
                actual = self.temperatures(result)
                self.compare_temperatures(actual, {p: reference[p] for p in actual}, 1e-8)
                gids = self.diagnostics(result, fractions, nz=1, trace_nz=1)
                results.append((actual, gids))
        if len(results) == 2:
            self.compare_temperatures(results[0][0], results[1][0], 0., PARALLEL_RTOL)
            self.assertEqual(results[0][1].keys(), results[1][1].keys())
            for gid in results[0][1]:
                np.testing.assert_allclose(results[0][1][gid][1], results[1][1][gid][1], rtol=PARALLEL_RTOL, atol=0)
                self.assertAlmostEqual(results[0][1][gid][0], results[1][1][gid][0], delta=1e-12)

    def test_defaults_and_fraction_endpoints(self):
        for f in (0., .3, 1.):
            with self.subTest(f=f):
                stack = copy.deepcopy(self.stack)
                stack['layers'][0]['copper']['fraction'] = f
                result = self.run_case('default-'+str(f), stack)
                self.one_dimensional(result, f)
                fields = {v['name'] for v in result[0]['fields'] if not v.get('reserved')}
                self.assertTrue(set(LEGACY_FIELDS+DIAGNOSTICS) <= fields,
                                f'default output missing {set(LEGACY_FIELDS+DIAGNOSTICS)-fields}')
                data = result[1]['hex']
                self.assertTrue(set(LEGACY_FIELDS[:2]) <= data['point'].keys())
                self.assertTrue(set(LEGACY_FIELDS[2:]) <= data['cell'].keys())
                self.diagnostics(result, np.full((2, 2), f))

    def test_explicit_diagnostics_and_u_only_selection(self):
        for fields in ([*DIAGNOSTICS, 'elem_gid'], ['U']):
            with self.subTest(fields=fields):
                case = copy.deepcopy(self.case)
                case['output']['fields'] = fields
                result = self.run_case('selected-'+fields[0], case=case)
                manifest, blocks = result
                selected = {f['name'] for f in manifest['fields'] if not f.get('reserved')}
                self.assertEqual(selected, set(fields))
                self.assertEqual(set(blocks['hex']['point']), set(fields) & {'T', 'U'})
                self.assertEqual(set(blocks['hex']['cell']), set(fields) - {'T', 'U'})
                if fields[0] == 'copper_fraction':
                    cells = blocks['hex']['cell']
                    for e, gid in enumerate(cells['elem_gid']):
                        f = .3 if int(gid) % 4 < 2 else 0.
                        self.assertAlmostEqual(cells['copper_fraction'][e], f, delta=1e-12)
                        np.testing.assert_allclose(cells['conductivity_diag'][e], conductivity(f),
                                                   rtol=1e-14, atol=0)

    def test_mechanical_only_supplied_conductivity_is_reported(self):
        case = copy.deepcopy(self.case)
        case['physics'] = [dict(id='mech', solver='fem.solid')]
        case['loads'] = [dict(type='uniform_temperature', T_ref=300., T_target=350.)]
        result = self.run_case('mechanical-only', case=case)
        self.diagnostics(result, np.full((2, 2), .3))

    def test_mechanical_only_missing_conductivity_omits_undefined_diagnostic(self):
        case = copy.deepcopy(self.case)
        case['physics'] = [dict(id='mech', solver='fem.solid')]
        case['loads'] = [dict(type='uniform_temperature', T_ref=300., T_target=350.)]
        for material in self.materials:
            del material['conductivity']
        result = self.run_case('mechanical-missing-conductivity', case=case)
        fields = {f['name'] for f in result[0]['fields']}
        self.assertIn('copper_fraction', fields)
        self.assertNotIn('conductivity_diag', fields)
        cells = result[1]['hex']['cell']
        self.assertNotIn('conductivity_diag', cells)
        for gid, f in zip(cells['elem_gid'], cells['copper_fraction']):
            self.assertAlmostEqual(float(f), .3 if int(gid) % 4 < 2 else 0., delta=1e-12)

    def test_mechanical_only_supplied_conductivity_is_validated(self):
        case = copy.deepcopy(self.case)
        case['physics'] = [dict(id='mech', solver='fem.solid')]
        case['loads'] = [dict(type='uniform_temperature', T_ref=300., T_target=350.)]
        self.materials[0]['conductivity'] = -1.
        self.run_case('mechanical-invalid-conductivity', case=case, expected=14,
                      diagnostic='conductivity')

    def test_sidecar_duplicate_layers_and_inconsistent_database_units_rejected(self):
        stack, binary = self.polygons(self.stack, np.full((2, 2), .3))
        stack['layers'][1]['kind'] = 'Patterned'
        stack['layers'][1]['copper'] = dict(metal='cu', polygons=dict(file='second.polyset', layer='trace'))
        for kind in ('duplicate', 'dbu'):
            with self.subTest(kind=kind):
                second = binary if kind == 'duplicate' else binary[:8] + struct.pack('<d', 2e-6) + binary[16:]
                stack['external_refs'] = [dict(path=name, sha256=hashlib.sha256(data).hexdigest())
                                         for name, data in [('trace.polyset', binary), ('second.polyset', second)]]
                self.run_case('sidecar-'+kind, stack,
                              polyset={'trace.polyset': binary, 'second.polyset': second}, expected=14,
                              diagnostic='duplicate polyset layer name' if kind == 'duplicate' else 'same database unit')

    def test_neither_copper_representation_rejected(self):
        stack = copy.deepcopy(self.stack)
        del stack['layers'][0]['copper']['fraction']
        self.run_case('neither', stack, expected=14, diagnostic='/copper')

    def test_region_polygon_remains_cut_unsupported(self):
        stack = copy.deepcopy(self.stack)
        stack['layers'][1]['regions'] = [dict(name='insert', material='cu',
            footprint=dict(box=[.01, .01, .03, .03], polygon=[[.01, .01], [.03, .01], [.02, .03]]))]
        self.run_case('region-polygon', stack, expected=13,
                      diagnostic='internal material cuts are not supported')

    def test_duplicate_diagnostic_selection_rejected(self):
        case = copy.deepcopy(self.case)
        case['output']['fields'] = ['copper_fraction', 'copper_fraction']
        self.run_case('duplicate-fields', case=case, expected=14, diagnostic='invalid output field')

    def test_padded_columns_and_mpi_global_gid_diagnostics(self):
        case = copy.deepcopy(self.case)
        case['mesh'].update(nx=3, ny=3, levels=2)
        # Hierarchy padding subdivides the same outline into 4x4 columns.
        # Align the synthetic stripe with those final columns; keep every GID
        # and fraction assertion below unchanged.
        fractions = np.zeros((4, 4))
        fractions[2, 2] = .3
        stack, binary = self.polygons(self.stack, fractions, nx=4, ny=4)
        for ranks in (1, 2):
            with self.subTest(ranks=ranks):
                result = self.run_case(f'padded-{ranks}', stack, case, ranks, binary)
                data = result[1]['hex']
                self.assertEqual(result[0]['mesh']['padded_cells'], 28)
                self.assertEqual(len(data['cell']['elem_gid']), 64)
                for name in DIAGNOSTICS:
                    self.assertIn(name, data['cell'])
                xy = columns(4, 4)
                for e, gid in enumerate(data['cell']['elem_gid']):
                    x, y = xy[int(gid)//4]
                    expected = .3 if (x, y) == (2, 2) and int(gid) % 4 < 2 else 0.
                    self.assertAlmostEqual(data['cell']['copper_fraction'][e], expected, delta=1e-12)
                    k = conductivity(expected)
                    np.testing.assert_allclose(data['cell']['conductivity_diag'][e], k, rtol=1e-14, atol=0)

    def test_cut_and_hex_fallback_diagnostics_follow_parent_gid(self):
        stack = copy.deepcopy(self.stack)
        stack['outline']['polygon'] = [[.004, 0], [.036, 0], [.04, .004], [.04, .036],
                                       [.036, .04], [.004, .04], [0, .036], [0, .004]]
        case = copy.deepcopy(self.case)
        case['mesh'].update(nx=4, ny=4, cut=dict(stabilization='ghost_penalty', quad_degree=6, gamma_g=.1))
        # A fixed face anchors the constant thermal state; the embedded face stays Robin.
        # This fixture checks diagnostic export without an unrelated mechanics residual.
        case['physics'] = [dict(id='thermal', solver='fem.conduction', mode='steady')]
        case.pop('constraints')
        case['loads'] = [dict(type='fixed_temperature', target='face:zmin', T=350.),
                         dict(type='convection', target='boundary:embedded', h=10., T_inf=350.)]
        for fallback, ranks in itertools.product((False, True), (1, 2)):
            with self.subTest(fallback=fallback, ranks=ranks):
                result = self.run_case(f'cut-{fallback}-{ranks}', stack, case, ranks=ranks, fallback=fallback)
                blocks = result[1]
                self.assertEqual(bool(len(blocks['cut']['cell']['elem_gid'])), not fallback)
                self.assertGreater(len(blocks['eb_surface']['cell']['elem_gid']), 0)
                self.diagnostics(result, np.full((4, 4), .3), nx=4, ny=4, cut=True)
                for block in blocks.values():
                    if len(block['points']):
                        self.assertTrue(np.all(np.isfinite(block['point']['T'])))
                        np.testing.assert_allclose(block['point']['T'], 350, rtol=0, atol=1e-8)


if __name__ == '__main__':
    unittest.main(argv=[sys.argv[0]], verbosity=2)
