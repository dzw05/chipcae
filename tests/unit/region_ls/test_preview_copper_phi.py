"""Acceptance for preview's raw column copper fractions (synthetic inputs only)."""

import hashlib
import json
import os
from pathlib import Path
import resource
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import unittest

import h5py
import numpy as np


def fraction_stack(copper=True):
    layers = [dict(name='core', thickness=.0004, kind='Homogenized',
                   material='fr4_eq', n_elem_min=2, grading=1)]
    for name, value in [('L1', .25), ('L2', .6)]:
        layer = dict(name=name, thickness=.000035, kind='Patterned' if copper else 'Homogenized',
                     material='fr4_eq', n_elem_min=1, grading=1)
        if copper:
            layer['copper'] = dict(metal='cu', fraction=value)
        layers.append(layer)
    return dict(id='preview_copper', unit='m', symmetry='none', package_type='synthetic',
                outline=dict(box=[0, 0, .004, .003], polygon=[
                    [0, 0], [.004, 0], [.004, .0025], [.0035, .003], [0, .003]]),
                layers=layers)


def diagnostics(result):
    return (f'\ncommand: {shlex.join(result.args)}\nexit: {result.returncode}'
            f'\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}')


class PreviewCopperPhi(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = Path(os.environ['CHIPCAE_BINARY_DIR'])
        cls.preview = cls.build / 'preview' / 'chipcae-preview'

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='preview-copper-phi-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)

    def invoke(self, name, stack, nx=4, ny=4, launcher=()):
        source = self.root / f'{name}.json'
        source.write_text(json.dumps(stack), encoding='utf-8')
        stem = self.root / name
        command = [*launcher, str(self.preview), str(source), '--nx', str(nx), '--ny', str(ny),
                   '--levels', '1', '--max-ratio', '10', '-o', str(stem)]
        env = dict(os.environ, OMPI_ALLOW_RUN_AS_ROOT='1', OMPI_ALLOW_RUN_AS_ROOT_CONFIRM='1')
        start = time.perf_counter()
        try:
            result = subprocess.run(command, capture_output=True, text=True, env=env, timeout=8)
        except subprocess.TimeoutExpired as error:
            self.fail(f'command timed out: {shlex.join(command)}'
                      f'\nstdout:\n{error.stdout}\nstderr:\n{error.stderr}')
        seconds = time.perf_counter() - start
        try:
            events = [json.loads(line) for line in result.stdout.splitlines() if line.strip()]
        except json.JSONDecodeError as error:
            self.fail(f'invalid stdout JSON: {error}{diagnostics(result)}')
        return result, events, stem, seconds

    def measure(self, label, runs):
        summaries = [event for _, events, _, _ in runs for event in events
                     if event.get('stage') == 'success']
        rss = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
        if sys.platform == 'darwin':
            rss /= 1024
        print(f'MEASURE {label}: ' + json.dumps(dict(
            cells=sum(s['cell_count'] for s in summaries),
            cut_cells=sum(s['cut_cell_count'] for s in summaries),
            seconds=sum(run[3] for run in runs), children_maxrss_cumulative_mib=rss / 1024)),
            flush=True)

    def success(self, run):
        result, events, _, _ = run
        context = diagnostics(result)
        self.assertEqual(result.returncode, 0, context)
        found = [event for event in events if event.get('stage') == 'success']
        self.assertEqual(len(found), 1, context)
        return found[0]

    def read_grid(self, run):
        summary = self.success(run)
        context = diagnostics(run[0])
        with h5py.File(str(run[2]) + '.vtkhdf', 'r') as file:
            grid = file['VTKHDF']
            count = int(grid['NumberOfCells'][0])
            corners = grid['Points'][:][grid['Connectivity'][:].reshape(-1, 8)]
            cd = grid['CellData']
            for name in ('attribute', 'body', 'cell_kind', 'layer_id', 'material_id', 'rank',
                         'region', 'vol_frac', 'copper_phi'):
                self.assertIn(name, cd, f'missing CellData/{name}{context}')
                self.assertEqual(cd[name].shape, (count,), context)
                self.assertEqual(cd[name].dtype, np.dtype(
                    'float64' if name in ('vol_frac', 'copper_phi') else 'int32'), context)
            phi = cd['copper_phi'][:]
            kind = cd['cell_kind'][:]
            names = [name.decode('utf-8') if isinstance(name, bytes) else str(name)
                     for name in grid['FieldData/layer_names'][:]]
            ids = cd['layer_id'][:]
            self.assertTrue(np.all((ids >= 1) & (ids <= len(names))), context)
            layers = np.asarray([names[index - 1] for index in ids])
        self.assertEqual(count, summary['cell_count'], context)
        self.assertEqual(len(corners), count, context)
        self.assertEqual(int(np.count_nonzero(kind == 1)), summary['cut_cell_count'], context)
        self.assertTrue(np.all(np.isfinite(phi) & (phi >= 0) & (phi <= 1)), context)
        return phi, layers, kind, corners

    def check_fraction(self, grid):
        phi, layers, kind, _ = grid
        self.assertGreater(np.count_nonzero(kind == 1), 0)
        for name, expected in [('core', 0.), ('L1', .25), ('L2', .6)]:
            mask = layers == name
            self.assertTrue(np.any(mask), f'no cells in {name}')
            # Exact equality is intentional: these are the doubles supplied in the stack.
            self.assertTrue(np.all(phi[mask] == expected), f'wrong raw fraction in {name}')
            if name != 'core':
                cut = mask & (kind == 1)
                self.assertTrue(np.any(cut), f'no cut cells in {name}')
                self.assertTrue(np.all(phi[cut] == expected), f'cut fraction rescaled in {name}')

    def test_fraction_layers(self):
        run = self.invoke('fraction_layers', fraction_stack())
        self.measure('fraction_layers', [run])
        self.check_fraction(self.read_grid(run))

    def test_no_copper_zero(self):
        run = self.invoke('no_copper_zero', fraction_stack(False))
        self.measure('no_copper_zero', [run])
        phi, layers, _, _ = self.read_grid(run)
        self.assertEqual(set(layers), {'core', 'L1', 'L2'})
        self.assertTrue(np.all(phi == 0.), 'non-copper cells must have exact zero copper_phi')

    def test_polygon_layer_matches_area_reference(self):
        # Integer nm coordinates align with the normalized raster grid. The large
        # rectangle covers full cells; separated smaller rectangles cross cell edges.
        rectangles_nm = [(0, 0, 1000000, 1000000),
                         (1250000, 125000, 1750000, 375000),
                         (2250000, 125000, 2750000, 375000),
                         (3250000, 125000, 3750000, 375000),
                         (1250000, 1125000, 1750000, 1375000),
                         (2250000, 1125000, 2750000, 1375000),
                         (3250000, 1125000, 3750000, 1375000),
                         (1250000, 2125000, 1750000, 2375000)]
        data = bytearray(b'CCPOLY01')
        data.extend(struct.pack('<dII', 1e-9, 1, 2))
        data.extend(b'L1')
        data.extend(struct.pack('<I', len(rectangles_nm)))
        data.extend(struct.pack('<9q', *range(0, 33, 4)))
        for x0, y0, x1, y1 in rectangles_nm:
            data.extend(struct.pack('<8q', x0, y0, x1, y0, x1, y1, x0, y1))
        sidecar = self.root / 'copper.polyset'
        sidecar.write_bytes(data)
        stack = fraction_stack()
        stack['outline'] = dict(box=[0, 0, .004, .003])
        stack['layers'] = stack['layers'][:2]
        # Keep 100 um core cells adjacent to 17.5 um copper-layer cells
        # within max_ratio=10, while exercising multiple z cells in L1.
        stack['layers'][0]['n_elem_min'] = 4
        stack['layers'][1]['n_elem_min'] = 2
        stack['layers'][1]['copper'] = dict(
            metal='cu', polygons=dict(file=sidecar.name, layer='L1'))
        stack['external_refs'] = [dict(path=sidecar.name, sha256=hashlib.sha256(data).hexdigest())]
        run = self.invoke('polygon_layer', stack, nx=8, ny=6)
        self.measure('polygon_layer', [run])
        if run[0].returncode != 0 and any(
                event.get('stage') == 'error' and
                'polygon copper rasterization requires CHIPCAE_ENABLE_ECAD'
                in event.get('message', '') for event in run[1]):
            reason = 'polygon copper rasterization requires CHIPCAE_ENABLE_ECAD'
            print(f'SKIP: {reason}', flush=True)
            self.skipTest(reason)
        phi, layers, _, corners = self.read_grid(run)
        self.assertTrue(np.any(layers == 'core'))
        self.assertTrue(np.all(phi[layers == 'core'] == 0.))
        mask = layers == 'L1'
        self.assertTrue(np.any(mask))
        lower, upper = corners[mask].min(axis=1), corners[mask].max(axis=1)
        area = np.prod((upper - lower)[:, :2], axis=1)
        self.assertTrue(np.all(area > 0))
        reference = np.zeros(len(area))
        for x0, y0, x1, y1 in rectangles_nm:
            lo = np.maximum(lower[:, :2], np.asarray([x0, y0]) * 1e-9)
            hi = np.minimum(upper[:, :2], np.asarray([x1, y1]) * 1e-9)
            reference += np.prod(np.maximum(hi - lo, 0), axis=1) / area
        values = phi[mask]
        maximum_error = float(np.max(np.abs(values - reference)))
        print(f'polygon_layer maximum_area_reference_error={maximum_error:.17g}', flush=True)
        self.assertLessEqual(maximum_error, 1e-12)
        for category in (values == 0., (values > 0.) & (values < 1.), values == 1.):
            self.assertTrue(np.any(category), 'fixture must exercise empty, partial and full copper')
        columns = {}
        for lo, hi, value in zip(lower, upper, values):
            key = tuple(np.round(np.r_[lo[:2], hi[:2]], 12))
            columns.setdefault(key, []).append(value)
        for key, column in columns.items():
            self.assertGreaterEqual(len(column), 2, f'missing L1 z subdivision at {key}')
            bits = np.asarray(column, dtype=np.float64).view(np.uint64)
            self.assertTrue(np.all(bits == bits[0]), f'copper varies along z at {key}')

    def test_mpi_partition_equal(self):
        cache = (self.build / 'CMakeCache.txt').read_text(encoding='utf-8')
        mpi = shutil.which('mpiexec') or shutil.which('mpirun')
        if 'CHIPCAE_ENABLE_MPI:BOOL=ON' not in cache or not mpi:
            reason = 'MPI-enabled preview and mpiexec/mpirun required'
            print(f'SKIP: {reason}', flush=True)
            self.skipTest(reason)
        serial = self.invoke('fraction_np1', fraction_stack())
        parallel = self.invoke('fraction_np2', fraction_stack(), launcher=[mpi, '-n', '2'])
        self.measure('mpi_partition_equal', [serial, parallel])
        summaries = [self.success(run) for run in (serial, parallel)]
        self.assertEqual(summaries[0]['mesh_sha256'], summaries[1]['mesh_sha256'])
        records = []
        for run in (serial, parallel):
            grid = self.read_grid(run)
            self.check_fraction(grid)
            phi, layers, _, corners = grid
            keys = [(str(layer), *np.round(center, 12))
                    for layer, center in zip(layers, corners.mean(axis=1))]
            self.assertEqual(len(set(keys)), len(keys), 'non-unique cell centers')
            records.append(sorted(zip(keys, phi.view(np.uint64).tolist())))
        self.assertEqual(records[0], records[1], 'MPI cell keys or copper bits differ')


if __name__ == '__main__':
    unittest.main(argv=[sys.argv[0]], verbosity=2)
