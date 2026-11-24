"""Acceptance for preview's ordered copper area conservation report."""

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
import unittest

import h5py
import numpy as np


MESSAGE_KEYS = [
    'grid', 'max_ratio', 'n0', 'nx0', 'ny0', 'L', 'padded_cells', 'void_cells',
    'nz', 'node_count', 'cell_count', 'cut_cell_count', 'mesh_sha256',
    'cell_budget', 'min_elems_per_layer', 'copper_area_m2',
]


def fraction_stack(copper=True):
    layers = [dict(name='core', thickness=.0004, kind='Homogenized',
                   material='fr4_eq', n_elem_min=4, grading=1)]
    for name, fraction in [('L1', .25), ('L2', .6)]:
        layer = dict(name=name, thickness=.000035,
                     kind='Patterned' if copper else 'Homogenized',
                     material='fr4_eq', n_elem_min=2, grading=1)
        if copper:
            layer['copper'] = dict(metal='cu', fraction=fraction)
        layers.append(layer)
    return dict(id='copper-area', unit='m', symmetry='none', package_type='synthetic',
                outline=dict(box=[0, 0, .004, .003]), layers=layers)


def signed_area(rings, dbu):
    # Python integers keep the shoelace sum exact until conversion to m².
    twice_area = sum(x * v - u * y for ring in rings
                     for (x, y), (u, v) in zip(ring, ring[1:] + ring[:1]))
    return twice_area / 2 * dbu**2


def decode_polyset(path):
    data = path.read_bytes()
    if data[:8] != b'CCPOLY01':
        raise AssertionError(f'invalid polyset magic: {path}')
    offset = 8

    def unpack(fmt):
        nonlocal offset
        result = struct.unpack_from(fmt, data, offset)
        offset += struct.calcsize(fmt)
        return result

    dbu, count = unpack('<dI')
    layers = {}
    for _ in range(count):
        length, = unpack('<I')
        name = data[offset:offset + length].decode('utf-8')
        offset += length
        ring_count, = unpack('<I')
        offsets = unpack(f'<{ring_count + 1}q')
        coordinates = unpack(f'<{2 * offsets[-1]}q')
        points = list(zip(coordinates[::2], coordinates[1::2]))
        layers[name] = [points[a:b] for a, b in zip(offsets, offsets[1:])]
    if offset != len(data):
        raise AssertionError(f'trailing polyset data: {path}')
    return dbu, layers


def diagnostics(result):
    return (f'\ncommand: {shlex.join(result.args)}\nexit: {result.returncode}'
            f'\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}')


class PreviewCopperArea(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = Path(os.environ['CHIPCAE_BINARY_DIR'])
        cls.cache = (cls.build / 'CMakeCache.txt').read_text(encoding='utf-8')
        cls.preview = cls.build / 'preview' / 'chipcae-preview'

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='preview-copper-area-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.env = dict(os.environ, OMPI_ALLOW_RUN_AS_ROOT='1',
                        OMPI_ALLOW_RUN_AS_ROOT_CONFIRM='1')

    def require_ecad(self):
        if 'CHIPCAE_ENABLE_ECAD:BOOL=ON' not in self.cache:
            self.skipTest('polygon cases require CHIPCAE_ENABLE_ECAD=ON')

    def source(self, name, stack):
        path = self.root / f'{name}.json'
        path.write_text(json.dumps(stack), encoding='utf-8')
        return path

    def invoke(self, name, source, nx=4, ny=4, launcher=(), unpadded=False):
        stem = self.root / name
        command = [*launcher, str(self.preview), str(source), '-o', str(stem),
                   '--nx', str(nx), '--ny', str(ny), '--levels', '1']
        try:
            result = subprocess.run(command, capture_output=True, text=True,
                                    env=self.env, timeout=8)
        except subprocess.TimeoutExpired as error:
            self.fail(f'command timed out: {shlex.join(command)}'
                      f'\nstdout:\n{error.stdout}\nstderr:\n{error.stderr}')
        context = diagnostics(result)
        self.assertEqual(result.returncode, 0, context)
        try:
            lines = [line for line in result.stdout.splitlines() if line.strip()]
            events = [json.loads(line) for line in lines]
            pairs = [json.loads(line, object_pairs_hook=list) for line in lines]
        except json.JSONDecodeError as error:
            self.fail(f'invalid stdout JSON: {error}{context}')
        found = [event for event in events if event.get('stage') == 'success']
        ordered = [event for event in pairs if ('stage', 'success') in event]
        self.assertEqual(len(found), 1, context)
        self.assertEqual(len(ordered), 1, context)
        summary, event = found[0], ordered[0]
        message = json.loads(summary['message'])
        message_pairs = json.loads(summary['message'], object_pairs_hook=list)
        self.assertEqual(set(summary), {'stage', 'progress', 'message', *message}, context)
        for key, value in message.items():
            self.assertEqual(summary[key], value, f'{key}{context}')
        if unpadded:
            self.assertEqual(summary['padded_cells'], 0, context)
        # Fail explicitly on the absent feature, before reading its values.
        self.assertIn('copper_area_m2', message, context)
        self.assertEqual([key for key, _ in message_pairs], MESSAGE_KEYS, context)
        self.assertEqual([key for key, _ in event],
                         ['stage', 'progress', *MESSAGE_KEYS, 'message'], context)
        report = message['copper_area_m2']
        self.assertIsInstance(report, dict, context)
        report_pairs = dict(message_pairs)['copper_area_m2']
        self.assertEqual(dict(event)['copper_area_m2'], report_pairs, context)
        self.assertEqual([name for name, _ in report_pairs], list(report), context)
        for name, values in report_pairs:
            self.assertEqual([key for key, _ in values], ['polygons', 'raster'], context)
            for key, value in values:
                self.assertIn(type(value), (int, float), context)
                self.assertTrue(math.isfinite(value), f'{name}.{key}{context}')
        return summary, report_pairs, stem

    def assert_area(self, actual, reference):
        error = abs(actual - reference)
        if reference:
            error /= max(abs(reference), 1e-30)
        self.assertLessEqual(error, 1e-12, f'{actual=}, {reference=}')

    def fixture(self):
        self.assertIn('CHIPCAE_SOURCE_DIR', os.environ)
        source = Path(os.environ['CHIPCAE_SOURCE_DIR']) / 'schema/examples/copper_polygons.stack.json'
        self.assertTrue(source.is_file(), str(source))
        stack = json.loads(source.read_text(encoding='utf-8'))
        polygons = stack['layers'][0]['copper']['polygons']
        sidecar = source.parent / polygons['file']
        self.assertTrue(sidecar.is_file(), str(sidecar))
        dbu, layers = decode_polyset(sidecar)
        return source, signed_area(layers[polygons['layer']], dbu)

    def test_no_copper_empty_object(self):
        summary, pairs, _ = self.invoke('no-copper', self.source('no-copper', fraction_stack(False)))
        self.assertEqual(summary['copper_area_m2'], {})
        self.assertEqual(pairs, [])

    def test_fraction_layers_bitwise(self):
        summary, _, _ = self.invoke(
            'fraction', self.source('fraction', fraction_stack()), unpadded=True)
        self.assertEqual(summary['padded_cells'], 0)
        report = summary['copper_area_m2']
        self.assertEqual(list(report), ['L1', 'L2'])
        for name, fraction in [('L1', .25), ('L2', .6)]:
            values = report[name]
            self.assertEqual(struct.pack('<d', values['polygons']),
                             struct.pack('<d', values['raster']))
            for value in values.values():
                self.assert_area(value, fraction * .004 * .003)

    def test_polygon_conservation_and_pattern(self):
        self.require_ecad()
        outer = (250000, 250000, 2750000, 2250000)
        hole = (1250000, 750000, 1750000, 1250000)
        rings = []
        for bounds, clockwise in [(outer, False), (hole, True)]:
            x0, y0, x1, y1 = bounds
            ring = [(x0, y0), (x1, y0), (x1, y1), (x0, y1)]
            rings.append(ring[::-1] if clockwise else ring)
        data = bytearray(b'CCPOLY01')
        data.extend(struct.pack('<dII', 1e-9, 1, 2))
        data.extend(b'L1')
        data.extend(struct.pack('<I3q', 2, 0, 4, 8))
        for ring in rings:
            for point in ring:
                data.extend(struct.pack('<2q', *point))
        sidecar = self.root / 'copper.polyset'
        sidecar.write_bytes(data)
        stack = fraction_stack()
        stack['layers'] = stack['layers'][:2]
        stack['layers'][1]['copper'] = dict(
            metal='cu', polygons=dict(file=sidecar.name, layer='L1'))
        stack['external_refs'] = [dict(path=sidecar.name, sha256=hashlib.sha256(data).hexdigest())]
        reference = signed_area(rings, 1e-9)
        self.assert_area(reference, 4.75e-6)
        summary, _, stem = self.invoke('polygon', self.source('polygon', stack), nx=8, ny=6)
        self.assertEqual(summary['padded_cells'], 0)
        report = summary['copper_area_m2']
        self.assertEqual(list(report), ['L1'])
        for value in report['L1'].values():
            self.assert_area(value, reference)
        # Read only geometry, layer identity and the raw fractions required by C3.
        with h5py.File(str(stem) + '.vtkhdf', 'r') as file:
            grid = file['VTKHDF']
            names = [name.decode('utf-8') if isinstance(name, bytes) else str(name)
                     for name in grid['FieldData/layer_names'][:]]
            mask = grid['CellData/layer_id'][:] == names.index('L1') + 1
            phi = grid['CellData/copper_phi'][:][mask]
            corners = grid['Points'][:][grid['Connectivity'][:].reshape(-1, 8)][mask]
        self.assertTrue(np.all(np.isfinite(phi) & (phi >= 0) & (phi <= 1)))
        columns = {}
        for points, value in zip(corners, phi):
            bounds = np.r_[points.min(axis=0)[:2], points.max(axis=0)[:2]]
            integer_bounds = tuple(int(round(float(x) / 1e-9)) for x in bounds)
            self.assertTrue(np.all(np.abs(bounds - np.asarray(integer_bounds) * 1e-9) <= 1e-15))
            columns.setdefault(integer_bounds, []).append(float(value))
        self.assertEqual(len(columns), 8 * 6)
        categories = [0, 0, 0]
        independent_sum = 0.
        for (x0, y0, x1, y1), values in columns.items():
            area = (x1 - x0) * (y1 - y0)
            self.assertGreater(area, 0)

            def intersection(rect):
                a, b, c, d = rect
                return max(0, min(x1, c) - max(x0, a)) * max(0, min(y1, d) - max(y0, b))

            covered = intersection(outer) - intersection(hole)
            expected = covered / area
            categories[0 if covered == 0 else 2 if covered == area else 1] += 1
            self.assertGreaterEqual(len(values), 2)
            for value in values:
                self.assertLessEqual(abs(value - expected), 1e-12)
                self.assertEqual(struct.pack('<d', value), struct.pack('<d', values[0]))
            # Each background column contributes once, irrespective of z subdivisions.
            independent_sum += expected * area * 1e-18
        self.assertTrue(all(count > 0 for count in categories), str(categories))
        self.assert_area(independent_sum, reference)
        self.assert_area(report['L1']['raster'], independent_sum)

    def test_example_fixture_conservation(self):
        self.require_ecad()
        source, reference = self.fixture()
        self.assert_area(reference, 2e-6)
        summary, _, _ = self.invoke('example', source, nx=10, ny=8)
        report = summary['copper_area_m2']
        self.assertEqual(list(report), ['F.Cu'])
        for value in report['F.Cu'].values():
            self.assert_area(value, reference)

    def test_mpi_partition_equal(self):
        mpi = shutil.which('mpiexec') or shutil.which('mpirun')
        if 'CHIPCAE_ENABLE_MPI:BOOL=ON' not in self.cache or not mpi:
            self.skipTest('MPI-enabled preview and mpiexec/mpirun required')
        if 'CHIPCAE_ENABLE_ECAD:BOOL=ON' in self.cache:
            source, _ = self.fixture()
            nx, ny = 10, 8
        else:
            source = self.source('mpi-fraction', fraction_stack())
            nx, ny = 4, 4
        runs = {}
        for ranks in (1, 2):
            with self.subTest(ranks=ranks):
                runs[ranks] = self.invoke(f'np{ranks}', source, nx, ny,
                                          launcher=[mpi, '-n', str(ranks)])
        if len(runs) == 2:
            self.assertEqual(runs[1][1], runs[2][1])
            self.assertEqual(runs[1][0]['mesh_sha256'], runs[2][0]['mesh_sha256'])


if __name__ == '__main__':
    unittest.main(argv=[sys.argv[0]], verbosity=2)
