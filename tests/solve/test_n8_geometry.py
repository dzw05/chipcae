"""N8 G4 acceptance: independent rational geometry and public preview output."""
import copy
from fractions import Fraction as F
from functools import lru_cache
import hashlib
import importlib
import json
import math
import os
from pathlib import Path
import re
import resource
import subprocess
import sys
import tempfile
import unittest

BOX = (0.0, 0.0, 1.0, 0.2)
POLYGON = ((0.0, 0.02), (0.9, 0.0), (1.0, 0.02), (1.0, 0.2), (0.0, 0.2))
TABLE = ((12, 4, 36, 12, 48), (24, 8, 168, 24, 192),
         (48, 16, 701, 50, 751), (96, 32, 2865, 102, 2967))
PREVIEW = None


def geometry():
    return importlib.import_module('n8_geometry')


def literal_equal(test, polygon, box):
    test.assertIsInstance(box, tuple)
    test.assertIsInstance(polygon, tuple)
    for point in polygon:
        test.assertIsInstance(point, tuple)
    test.assertEqual(box, BOX)
    test.assertEqual(polygon, POLYGON)


def rational(points):
    return tuple(tuple(F(str(v)) for v in p) for p in points)


def cross(a, b, p):
    return (b[0] - a[0]) * (p[1] - a[1]) - (b[1] - a[1]) * (p[0] - a[0])


def moments(points):
    pairs = list(zip(points, points[1:] + points[:1]))
    weights = [a[0] * b[1] - b[0] * a[1] for a, b in pairs]
    twice = sum(weights, F(0))
    if not twice:
        return F(0), None
    center = tuple(sum((a[k] + b[k]) * w for (a, b), w in zip(pairs, weights))
                   / (3 * twice) for k in (0, 1))
    return twice / 2, center


@lru_cache(None)
def clip_oracle(polygon, nx, ny, box=BOX):
    """Clip each background rectangle against the convex polygon's half planes."""
    polygon = rational(polygon)
    xmin, ymin, xmax, ymax = (F(str(x)) for x in box)
    hx, hy = (xmax - xmin) / nx, (ymax - ymin) / ny
    result = {}
    for j in range(ny):
        for i in range(nx):
            x, y = xmin + i * hx, ymin + j * hy
            clipped = ((x, y), (x + hx, y), (x + hx, y + hy), (x, y + hy))
            for a, b in zip(polygon, polygon[1:] + polygon[:1]):
                output = []
                for p, q in zip(clipped, clipped[1:] + clipped[:1]):
                    dp, dq = cross(a, b, p), cross(a, b, q)
                    if dp >= 0:
                        output.append(p)
                    if (dp < 0) != (dq < 0):
                        t = dp / (dp - dq)
                        output.append(tuple(p[k] + t * (q[k] - p[k]) for k in (0, 1)))
                clipped = tuple(output)
                if not clipped:
                    break
            result[i, j] = moments(clipped)[0] / (hx * hy)
    return result


def compare_mesh_to_oracle(vtkhdf_path, polygon, nx, ny, kz):
    import h5py
    import numpy as np
    oracle = clip_oracle(tuple(map(tuple, polygon)), nx, ny)
    nonempty = sum(v > 0 for v in oracle.values())
    cut = sum(0 < v < 1 for v in oracle.values())
    failures = []

    def check(tag, name, actual, expected, ok):
        if not ok:
            failures.append(f'[{tag}] {name}: actual={actual} expected={expected}')

    with h5py.File(vtkhdf_path, 'r') as file:
        grid = file['VTKHDF']
        count = int(np.sum(grid['NumberOfCells'][:]))
        points = grid['Points'][:]
        connectivity = grid['Connectivity'][:]
        offsets = grid['Offsets'][:]
        vf = grid['CellData/vol_frac'][:]
        layers = grid['CellData/layer_id'][:]
    check('count', 'cells', count, nonempty * 2 * kz, count == nonempty * 2 * kz)
    structural = (points.ndim == 2 and points.shape[1] == 3
                  and connectivity.ndim == 1 and offsets.shape == (count + 1,)
                  and vf.shape == layers.shape == (count,)
                  and np.issubdtype(connectivity.dtype, np.integer)
                  and np.issubdtype(offsets.dtype, np.integer))
    check('size', 'array shapes', (points.shape, connectivity.shape, offsets.shape,
                                 vf.shape, layers.shape), 'valid eight-node cells', structural)
    valid = (structural and offsets[0] == 0 and offsets[-1] == len(connectivity)
             and np.all(np.diff(offsets) == 8) and np.all(connectivity >= 0)
             and np.all(connectivity < len(points)) and np.all(np.isfinite(points)))
    check('size', 'indices/offsets/points', bool(valid), True, valid)
    if not valid:
        raise AssertionError('\n'.join(failures))
    corners = points[connectivity.reshape(count, 8)]
    lo, hi = corners.min(axis=1), corners.max(axis=1)
    h = np.array([1 / nx, .2 / ny, .05 / kz])
    size_error = float(np.max(np.abs((hi - lo) / h - 1)))
    check('size', 'max relative size error', size_error, '<=1e-12', size_error <= 1e-12)
    ij = np.rint(lo[:, :2] / h[:2]).astype(int)
    in_range = np.all((ij >= 0) & (ij < [nx, ny]))
    boundary_error = float(np.max(np.abs(lo[:, :2] - ij * h[:2]) / h[:2]))
    check('size', 'xy grid boundaries', (bool(in_range), boundary_error),
          '(True, <=1e-12)', in_range and boundary_error <= 1e-12)
    expected = np.array([float(oracle.get(tuple(index), F(0))) for index in ij])
    max_error = float(np.max(np.abs(vf - expected)))
    finite_fractions = np.all(np.isfinite(vf)) and np.all((vf > 0) & (vf <= 1))
    check('volfrac', 'max_volfrac_err', max_error, '<=1e-12',
          finite_fractions and max_error <= 1e-12)
    actual_cut = int(np.count_nonzero((vf > 0) & (vf < 1)))
    check('count', 'cut', actual_cut, cut * 2 * kz, actual_cut == cut * 2 * kz)
    ids = np.unique(layers)
    check('count', 'layer count', len(ids), 2, len(ids) == 2)
    volumes = vf * math.prod(h)
    volume = float(math.fsum(volumes))
    relative_volume = abs(volume / .019 - 1)
    check('volume', 'total', volume, '.019 (relative <=1e-12)', relative_volume <= 1e-12)
    ordered = sorted(ids, key=lambda lid: float(np.min(lo[layers == lid, 2])))
    for index, lid in enumerate(ordered):
        mask = layers == lid
        n = int(np.count_nonzero(mask))
        check('count', f'layer {lid}', n, nonempty * kz, n == nonempty * kz)
        layer_volume = float(math.fsum(volumes[mask]))
        check('volume', f'layer {lid}', layer_volume, '.0095 (relative <=1e-12)',
              abs(layer_volume / .0095 - 1) <= 1e-12)
        z_indices = np.rint((lo[mask, 2] - index * .05) / h[2]).astype(int)
        z_error = float(np.max(np.abs(lo[mask, 2] - (index * .05 + z_indices * h[2])) / h[2]))
        z_ok = np.all((z_indices >= 0) & (z_indices < kz)) and z_error <= 1e-12
        check('size', f'layer {lid} z positions', (z_indices.min(), z_indices.max(), z_error),
              f'0..{kz - 1}, error<=1e-12', z_ok)
        # Catch missing/duplicated cells even if aggregate counts happen to agree.
        keys = set(zip(ij[mask, 0], ij[mask, 1], z_indices))
        wanted = {(i, j, k) for (i, j), value in oracle.items() if value > 0 for k in range(kz)}
        check('count', f'layer {lid} unique grid cells', len(keys), n, len(keys) == n)
        # A moved boundary can change support while preserving every cell count.
        # Support mismatches concern the spatial fraction field, not its count.
        check('volfrac', f'layer {lid} occupied grid cells', (len(keys), len(keys - wanted)),
              (len(wanted), 0), keys == wanted)
    if failures:
        raise AssertionError('\n'.join(failures))
    return count, actual_cut, max_error, relative_volume


# Each wrapper starts with a fresh RUSAGE_CHILDREN peak and runs one preview only.
MEASURE = '''import json, resource, subprocess, sys, time
start = time.monotonic()
p = subprocess.run(sys.argv[1:], capture_output=True, text=True)
rss = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
print(json.dumps(dict(code=p.returncode, stderr=p.stderr, wall=time.monotonic()-start,
                     rss=rss / (1048576 if sys.platform == 'darwin' else 1024))))
'''


def run_preview(root, nx, ny, kz, executable=None):
    command = [str(executable or PREVIEW), str(root / 'stack.json'), '--nx', str(nx),
               '--ny', str(ny), '--levels', '0', '--max-ratio', '100', '--cell-budget',
               '1000000', '--min-elems-per-layer', str(kz), '-o', str(root / 'mesh')]
    measured = subprocess.run([sys.executable, '-c', MEASURE, *command],
                              capture_output=True, text=True, check=True)
    metrics = json.loads(measured.stdout)
    if metrics['code']:
        message = f"N8GEOM preview exit {metrics['code']}: {metrics['stderr']}"
        print(message, file=sys.stderr)
        raise AssertionError(message)
    if metrics['rss'] > 1024:
        raise AssertionError(f"preview RSS {metrics['rss']} MiB > 1024")
    return root / 'mesh.vtkhdf', metrics


def expected_files(nx, ny, kz, family):
    # Independent literals: never call a case/material builder to derive expectations.
    cards = []
    for name, conductivity, alpha in [('a', 2, 1e-5), ('b', 4, 3e-5)]:
        cards.append({'id': name, 'schema_version': '1', 'unit_system': 'SI',
                      'model': 'thermo_elastic', 'density': 2000, 'conductivity': conductivity,
                      'elastic': {'symmetry': 'isotropic', 'table': {
                          'temperatures': [300.0], 'columns': {'E': [25e9], 'nu': [.3]},
                          'interpolation': 'constant', 'extrapolation': 'clamp'}},
                      'thermal_expansion': {'symmetry': 'isotropic', 'cte_type': 'tangent',
                          'stress_free_temperature': 300, 'table': {'temperatures': [300.0],
                          'columns': {'alpha': [alpha]}, 'interpolation': 'constant',
                          'extrapolation': 'clamp'}},
                      'provenance': {'source_type': 'illustrative', 'confidence': 'illustrative'}})
    return {
        'stack.json': {'id': 'fvm_synthetic', 'unit': 'm', 'symmetry': 'none',
            'package_type': 'FCBGA', 'outline': {'box': list(BOX), 'polygon': list(map(list, POLYGON))},
            'layers': [{'name': name, 'kind': 'Explicit', 'material': name, 'thickness': .05,
                        'n_elem_min': kz, 'grading': 1} for name in ('a', 'b')]},
        'materials.json': {'schema': 'chipcae.material_library/1', 'materials': cards},
        'profile.json': {'schema': 'chipcae.run_profile/1.0', 'ranks': 1, 'backend': 'cpu', 'gpus': 0},
        'case.json': {'schema': 'chipcae.case/1.0', 'stack': {'ref': 'stack.json'},
            'materials': {'ref': 'materials.json'}, 'mesh': {'type': 'aligned_cartesian_cut',
            'nx': nx, 'ny': ny, 'levels': 0, 'min_elems_per_layer': kz, 'max_ratio': 100,
            'cell_budget': 1000000, 'cut': {'stabilization': 'ghost_penalty', 'quad_degree': 6,
                                        'gamma_g': .1}},
            'physics': [{'id': 'mech', 'solver': family + '.solid'}],
            'loads': [{'type': 'uniform_temperature', 'T_ref': 300, 'T_target': 350}],
            'constraints': {'rigid': '3-2-1'}, 'solver': {},
            'output': {'format': 'vtkhdf', 'warpage': {'standard': 'JESD22-B112',
                                                       'remove_rigid': 'best_fit_plane'}}}}


class N8GeometryTests(unittest.TestCase):
    def test_polygon_literal(self):
        g = geometry()
        literal_equal(self, g.N8_G4_POLYGON, g.N8_G4_BOX)

    def test_polygon_analytic(self):
        g = geometry()
        p = rational(g.N8_G4_POLYGON)
        area, center = moments(p)
        self.assertEqual(area, F(19, 100))
        self.assertEqual(center, (F(289, 570), F(299, 2850)))
        self.assertGreater(area, 0)
        for i, point in enumerate(p):
            self.assertGreater(cross(p[i - 1], point, p[(i + 1) % len(p)]), 0)
            self.assertTrue(0 <= point[0] <= 1 and 0 <= point[1] <= F(1, 5))
        self.assertIsNone(g.check_polygon(g.N8_G4_POLYGON, g.N8_G4_BOX))

    def test_clip_oracle_table(self):
        for nx, ny, full, cut, nonempty in TABLE:
            with self.subTest(nx=nx, ny=ny):
                values = clip_oracle(POLYGON, nx, ny).values()
                self.assertEqual((sum(v == 1 for v in values), sum(0 < v < 1 for v in values),
                                  sum(v > 0 for v in values)), (full, cut, nonempty))
        square = ((0, 0), (1, 0), (1, 1), (0, 1))
        self.assertEqual(clip_oracle(square, 1, 1, (0, 0, 1, 1)), {(0, 0): F(1)})

    def test_case_fields(self):
        g = geometry()
        helper = Path(__file__).resolve().parents[1] / 'integration/fvm_case_e2e/fvm_case_util.py'
        before = hashlib.sha256(helper.read_bytes()).digest()
        with tempfile.TemporaryDirectory() as tmp:
            saved = {}
            for family in ('fvm', 'fem'):
                for kz, nx, ny in ((2, 12, 4), (8, 24, 8)):
                    root = Path(tmp) / f'{family}-{kz}'
                    result = g.n8_g4_case(root, nx, ny, kz, family)
                    expected = expected_files(nx, ny, kz, family)
                    self.assertEqual(result, expected['case.json'])
                    self.assertEqual({p.name for p in root.iterdir()}, set(expected))
                    for name, value in expected.items():
                        self.assertEqual(json.loads((root / name).read_text()), value, name)
                    first = {p: p.read_bytes() for p in root.glob('*.json')}
                    self.assertEqual(g.n8_g4_case(root, nx, ny, kz, family), result)
                    self.assertEqual({p: p.read_bytes() for p in first}, first)
                    saved.update(first)
            self.assertEqual({p: p.read_bytes() for p in saved}, saved)
        self.assertEqual(hashlib.sha256(helper.read_bytes()).digest(), before)
        with tempfile.TemporaryDirectory() as tmp:
            result = g.n8_g4_case(Path(tmp), 12, 4, 2)
            self.assertEqual(result, expected_files(12, 4, 2, 'fvm')['case.json'])

    def test_preview_matches_oracle(self):
        g = geometry()
        for nx, ny, _, _, _ in TABLE:
            for kz in (4, 8):
                with self.subTest(nx=nx, ny=ny, kz=kz), tempfile.TemporaryDirectory() as tmp:
                    root = Path(tmp)
                    g.n8_g4_case(root, nx, ny, kz)
                    path, metrics = run_preview(root, nx, ny, kz)
                    stats = compare_mesh_to_oracle(path, POLYGON, nx, ny, kz)
                    own_rss = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
                    own_mib = own_rss / (1048576 if sys.platform == 'darwin' else 1024)
                    self.assertLessEqual(own_mib + metrics['rss'], 15360)
                    print('N8GEOM', nx, ny, kz, *stats, metrics['wall'], metrics['rss'], flush=True)

    def test_negative_polygon_rejected(self):
        g = geometry()
        cases = [([], BOX, 'fewer than 3 vertices'),
                 ([(0, 0), (1, 0)], BOX, 'fewer than 3 vertices'),
                 ([(0, 0), (1, 0), (math.nan, 0)], BOX, 'non-finite'),
                 (POLYGON + POLYGON[:1], BOX, 'repeated vertex'),
                 ([(0, 0), (1, .2), (1, 0), (0, .2)], BOX, 'self-intersecting'),
                 ([(0, 0), (.5, 0), (1, 0), (1, .2), (0, .2)], BOX, 'collinear vertex'),
                 ([(0, 0), (1.1, .1), (1, .2), (0, .2)], BOX, 'outside box'),
                 (POLYGON[::-1], BOX, 'clockwise'),
                 (POLYGON, (0, 0, 0, .2), 'invalid box'),
                 ([(0, 0), (1,), (0, .2)], BOX, 'invalid vertex'),
                 ([(0, 0), ('bad', 0), (0, .2)], BOX, 'invalid vertex'),
                 ([(0, 0), (1, 0), (0, math.inf)], BOX, 'non-finite'),
                 ([(0, 0), (1, 0), (0, -math.inf)], BOX, 'non-finite'),
                 # Nonadjacent endpoint contact; no consecutive collinear vertices.
                 ([(0, 0), (1, 0), (.5, .1), (.5, 0), (0, .2)], BOX, 'self-intersecting'),
                 # Nonadjacent horizontal edges overlap.
                 ([(0, 0), (.8, 0), (.9, .1), (.6, 0), (.2, 0), (0, .2)], BOX,
                  'self-intersecting'),
                 ([], (0, 0, 0, 1), 'invalid box'),
                 ([(0,)], BOX, 'invalid vertex'),
                 ([(2, 0), (2, 0), (0, math.nan)], BOX, 'non-finite'),
                 ([(2, 0), (2, 0), (0, .2)], BOX, 'repeated vertex'),
                 ([(0, 0), (1, 0), (2, 0), (0, .2)], BOX, 'outside box')]
        for box in ((0, 0, 1), (0, 0, math.inf, 1), (0, 0, 1, math.nan),
                    (0, .2, 1, 0), ('bad', 0, 1, .2)):
            cases.append((POLYGON, box, 'invalid box'))
        for points, box, reason in cases:
            with self.subTest(points=points, box=box), self.assertRaisesRegex(
                    ValueError, '^n8_geometry: .*' + reason):
                g.check_polygon(points, box)
        # A legal concave polygon must remain acceptable.
        self.assertIsNone(g.check_polygon([(0, 0), (1, 0), (.5, .1), (1, .2), (0, .2)]))
        for index in range(3):
            for bad in (0, -1, 1.5, True):
                args = [12, 4, 4]
                args[index] = bad
                with self.subTest(args=args), tempfile.TemporaryDirectory() as tmp:
                    root = Path(tmp) / 'out'
                    with self.assertRaisesRegex(ValueError, '^n8_geometry:'):
                        g.n8_g4_case(root, *args)
                    self.assertFalse(root.exists())
        for family in ('FVM', 'Fem', '', 'other'):
            with self.subTest(family=family), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp) / 'out'
                with self.assertRaisesRegex(ValueError, '^n8_geometry:'):
                    g.n8_g4_case(root, 12, 4, 4, family)
                self.assertFalse(root.exists())

    def test_negative_rectangle_strip(self):
        sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'integration/fvm_case_e2e'))
        from fvm_case_util import bimaterial_strip
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            bimaterial_strip(root)
            path, _ = run_preview(root, 48, 16, 4)
            with self.assertRaises(AssertionError) as caught:
                compare_mesh_to_oracle(path, POLYGON, 48, 16, 4)
            message = str(caught.exception)
            self.assertIn('[count] cut: actual=0 expected=400', message)
            volume = re.search(r'\[volume\] total: actual=([^ ]+)', message)
            self.assertIsNotNone(volume, message)
            self.assertLessEqual(abs(float(volume.group(1)) / .02 - 1), 1e-12)

    def test_negative_moved_vertex(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            geometry().n8_g4_case(root, 48, 16, 4)
            stack_path = root / 'stack.json'
            stack = json.loads(stack_path.read_text())
            stack['outline']['polygon'][1][0] = .8
            stack_path.write_text(json.dumps(stack))
            path, _ = run_preview(root, 48, 16, 4)
            with self.assertRaises(AssertionError) as caught:
                compare_mesh_to_oracle(path, POLYGON, 48, 16, 4)
            message = str(caught.exception)
            self.assertIn('[volfrac]', message)
            self.assertNotIn('[count]', message)
            self.assertNotIn('[volume]', message)
            error = re.search(r'max_volfrac_err: actual=([^ ]+)', message)
            self.assertIsNotNone(error, message)
            self.assertGreater(float(error.group(1)), 1e-3)

    def test_negative_tampered_literal(self):
        polygon = copy.deepcopy(list(map(list, POLYGON)))
        polygon[0][1] = .0200001
        with self.assertRaises(AssertionError):
            literal_equal(self, tuple(map(tuple, polygon)), BOX)

    def test_missing_dependency_fails(self):
        script = str(Path(__file__).resolve())
        for dependency in ('h5py', 'numpy'):
            code = '''import importlib.abc, runpy, sys
dependency = sys.argv[1]
class Missing(importlib.abc.MetaPathFinder):
    def find_spec(self, fullname, path=None, target=None):
        if fullname.split('.')[0] == dependency:
            raise ImportError('injected missing dependency')
sys.meta_path.insert(0, Missing())
script, preview = sys.argv[2:]
sys.argv = [script, preview]
runpy.run_path(script, run_name='__main__')
'''
            p = subprocess.run([sys.executable, '-c', code, dependency, script, str(PREVIEW)],
                               capture_output=True, text=True)
            self.assertEqual(p.returncode, 1, p.stderr)
            self.assertIn('N8GEOM missing ' + dependency, p.stderr)
            self.assertNotIn('Ran ', p.stderr)
        with tempfile.TemporaryDirectory() as tmp:
            nonexec = Path(tmp) / 'nonexec'
            nonexec.write_text('not executable')
            nonexec.chmod(0o600)
            for arguments in ([], [str(Path(tmp) / 'absent')], [str(nonexec)]):
                p = subprocess.run([sys.executable, script, *arguments], capture_output=True, text=True)
                self.assertEqual(p.returncode, 1, p.stderr)
                self.assertIn('N8GEOM missing chipcae-preview', p.stderr)
                self.assertNotIn('Ran ', p.stderr)
        # A failing preview must propagate its exit code/stderr through the runner.
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            failing = root / 'fail-preview'
            failing.write_text('#!/bin/sh\necho intentional-preview-failure >&2\nexit 9\n')
            failing.chmod(0o700)
            code = ('import runpy, sys; from pathlib import Path; '
                    'm = runpy.run_path(sys.argv[1]); '
                    'm[\"run_preview\"](Path(sys.argv[2]), 12, 4, 4, Path(sys.argv[3]))')
            p = subprocess.run([sys.executable, '-c', code, script, str(root), str(failing)],
                               capture_output=True, text=True)
            self.assertEqual(p.returncode, 1, p.stderr)
            self.assertIn('N8GEOM preview exit 9: intentional-preview-failure', p.stderr)


def main():
    global PREVIEW
    for name in ('numpy', 'h5py'):
        try:
            importlib.import_module(name)
        except ImportError:
            print('N8GEOM missing ' + name, file=sys.stderr)
            return 1
    if len(sys.argv) != 2 or not Path(sys.argv[1]).is_file() or not os.access(sys.argv[1], os.X_OK):
        print('N8GEOM missing chipcae-preview', file=sys.stderr)
        return 1
    PREVIEW = Path(sys.argv[1]).resolve()
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(N8GeometryTests)
    return 0 if unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful() else 1


if __name__ == '__main__':
    sys.exit(main())
