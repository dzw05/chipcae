"""Serial acceptance for role-specific copper and opening area reports."""

import copy
import json
import math
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


MESSAGE_KEYS = [
    'grid', 'max_ratio', 'n0', 'nx0', 'ny0', 'L', 'padded_cells', 'void_cells',
    'nz', 'node_count', 'cell_count', 'cut_cell_count', 'mesh_sha256',
    'cell_budget', 'min_elems_per_layer', 'copper_area_m2',
]


def fraction_stack():
    layers = [dict(name='core', thickness=.0004, kind='Homogenized',
                   material='fr4_eq', n_elem_min=4, grading=1)]
    for name, fraction in [('L1', .25), ('SM', .4)]:
        layers.append(dict(name=name, thickness=.000035, kind='Patterned',
                           material='fr4_eq', n_elem_min=2, grading=1,
                           copper=dict(metal='cu', fraction=fraction)))
    return dict(id='copper-role', unit='m', symmetry='none', package_type='synthetic',
                outline=dict(box=[0, 0, .004, .003]), layers=layers)


def decode_polyset(path):
    data = path.read_bytes()
    if data[:8] != b'CCPOLY01':
        raise AssertionError(f'invalid polyset magic: {path}')
    offset = 8

    def unpack(fmt):
        nonlocal offset
        values = struct.unpack_from(fmt, data, offset)
        offset += struct.calcsize(fmt)
        return values

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
        raise AssertionError(f'trailing polyset bytes: {path}')
    return dbu, layers


class PreviewCopperRole(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = Path(os.environ['CHIPCAE_BINARY_DIR'])
        cls.source_root = Path(os.environ['CHIPCAE_SOURCE_DIR'])
        cls.preview = cls.build / 'preview/chipcae-preview'
        cls.cache = (cls.build / 'CMakeCache.txt').read_text(encoding='utf-8')
        for feature in ('CHIPCAE_ENABLE_JSONCONS', 'CHIPCAE_ENABLE_HDF5'):
            if f'{feature}:BOOL=OFF' in cls.cache:
                raise unittest.SkipTest(f'preview requires {feature}=ON')

    def setUp(self):
        self.started = time.perf_counter()
        self.addCleanup(lambda: print(
            f'CASE {self._testMethodName} wall_s={time.perf_counter() - self.started:.6f}',
            flush=True))
        temporary = tempfile.TemporaryDirectory(prefix='preview-copper-role-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.env = dict(os.environ, OMPI_ALLOW_RUN_AS_ROOT='1',
                        OMPI_ALLOW_RUN_AS_ROOT_CONFIRM='1')

    def write_stack(self, stack):
        path = self.root / 'stack.json'
        path.write_text(json.dumps(stack), encoding='utf-8')
        return path

    def invoke(self, source, nx=4, ny=4, success=True):
        stem = self.root / 'mesh'
        command = [str(self.preview), str(source), '-o', str(stem),
                   '--nx', str(nx), '--ny', str(ny), '--levels', '1',
                   '--max-ratio', '10']
        try:
            result = subprocess.run(command, capture_output=True, text=True,
                                    env=self.env, timeout=8)
        except subprocess.TimeoutExpired as error:
            self.fail(f'command timed out: {shlex.join(command)}\n'
                      f'stdout:\n{error.stdout}\nstderr:\n{error.stderr}')
        context = (f'\ncommand: {shlex.join(command)}\nexit: {result.returncode}'
                   f'\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}')
        events = []
        for line in result.stdout.splitlines():
            try:
                event = json.loads(line)
            except ValueError as error:
                self.fail(f'invalid stdout JSON: {error}{context}')
            self.assertIsInstance(event, dict, context)
            events.append((line, event))
        found = [(line, event) for line, event in events if event.get('stage') == 'success']
        if not success:
            self.assertNotEqual(result.returncode, 0, context)
            self.assertFalse(found, context)
            self.assertTrue(any(e.get('stage') == 'error' for _, e in events), context)
            return None
        self.assertEqual(result.returncode, 0, context)
        self.assertEqual(len(found), 1, context)
        line, event = found[0]
        message = json.loads(event['message'])
        pairs = json.loads(event['message'], object_pairs_hook=list)
        ordered = json.loads(line, object_pairs_hook=list)
        self.assertIsInstance(message, dict, context)
        for key, value in message.items():
            self.assertEqual(event[key], value, f'{key}{context}')
        self.assertEqual(ordered[2:-1], pairs, context)
        return line, message, pairs, ordered, stem

    def check_reports(self, result, stack):
        _, message, pairs, ordered, _ = result
        copper_layers = [layer for layer in stack['layers'] if 'copper' in layer]
        metal = [l['name'] for l in copper_layers
                 if l['copper'].get('role', 'metal') == 'metal']
        opening = [l['name'] for l in copper_layers
                   if l['copper'].get('role', 'metal') == 'opening']
        expected = MESSAGE_KEYS + (['opening_area_m2'] if opening else [])
        self.assertEqual([key for key, _ in pairs], expected)
        self.assertEqual([key for key, _ in ordered],
                         ['stage', 'progress', *expected, 'message'])
        if not opening:
            self.assertNotIn('opening_area_m2', message)
        pair_map = dict(pairs)
        for key, names in [('copper_area_m2', metal), ('opening_area_m2', opening)]:
            if key == 'opening_area_m2' and not opening:
                continue
            self.assertIn(key, message)
            self.assertIsInstance(message[key], dict)
            self.assertEqual(list(message[key]), names)
            self.assertEqual([name for name, _ in pair_map[key]], names)
            for name, members in pair_map[key]:
                self.assertEqual([k for k, _ in members], ['polygons', 'raster'])
                for member, value in members:
                    self.assertIn(type(value), (int, float))
                    self.assertTrue(math.isfinite(value), f'{name}.{member}')
        copper_keys = set(message['copper_area_m2'])
        opening_keys = set(message.get('opening_area_m2', {}))
        self.assertFalse(copper_keys & opening_keys)
        self.assertEqual(copper_keys | opening_keys, {l['name'] for l in copper_layers})

    def same_area(self, actual, expected):
        for key in ('polygons', 'raster'):
            self.assertEqual(float(actual[key]).hex(), float(expected[key]).hex(), key)

    def close_area(self, actual, expected):
        self.assertLessEqual(abs(actual - expected) / abs(expected), 1e-12)

    def phi(self, result):
        with h5py.File(str(result[4]) + '.vtkhdf', 'r') as file:
            return file['/VTKHDF/CellData/copper_phi'][...]

    def fixture(self, name):
        source = self.source_root / 'schema/examples' / name
        target = self.root / name
        shutil.copyfile(source, target)
        return target

    def test_metal_default_bytewise(self):
        source = self.fixture('fcbga_cu.stack.json')
        stack = json.loads(source.read_text(encoding='utf-8'))
        baseline = self.invoke(source)
        self.check_reports(baseline, stack)
        self.assertEqual(list(baseline[1]['copper_area_m2']), ['top_copper'])
        layer = next(l for l in stack['layers'] if l['name'] == 'top_copper')
        layer['copper'].update(role='metal', orientation='planar')
        explicit = self.invoke(self.write_stack(stack))
        self.check_reports(explicit, stack)
        self.assertEqual(baseline[0], explicit[0])

    def test_opening_fraction_routed(self):
        stack = fraction_stack()
        baseline = self.invoke(self.write_stack(stack))
        self.check_reports(baseline, stack)
        base_phi = self.phi(baseline)
        stack['layers'][2]['copper'].update(role='opening', metal='air_opening_reg')
        routed = self.invoke(self.write_stack(stack))
        routed_phi = self.phi(routed)
        self.assertEqual(routed[1]['padded_cells'], 0)
        self.check_reports(routed, stack)
        message, original = routed[1], baseline[1]
        self.same_area(message['opening_area_m2']['SM'], original['copper_area_m2']['SM'])
        self.same_area(message['copper_area_m2']['L1'], original['copper_area_m2']['L1'])
        area = message['opening_area_m2']['SM']
        self.assertEqual(float(area['polygons']).hex(), float(area['raster']).hex())
        x0, y0, x1, y1 = stack['outline']['box']
        for value in area.values():
            self.close_area(value, .4 * (x1 - x0) * (y1 - y0))
        for key in ('mesh_sha256', 'cell_count', 'node_count', 'nz'):
            self.assertEqual(message[key], original[key], key)
        self.assertEqual(routed_phi.dtype, base_phi.dtype)
        self.assertTrue(np.array_equal(routed_phi, base_phi))

    def test_all_opening(self):
        stack = fraction_stack()
        for layer in stack['layers'][1:]:
            layer['copper']['role'] = 'opening'
        # A zero-area opening must still cause the optional report to appear.
        zero = copy.deepcopy(stack)
        for layer in zero['layers'][1:]:
            layer['copper']['fraction'] = 0
        none = fraction_stack()
        for layer in none['layers'][1:]:
            del layer['copper']
            layer['kind'] = 'Homogenized'
        for name, case in [('all', stack), ('zero', zero), ('none', none)]:
            with self.subTest(case=name):
                result = self.invoke(self.write_stack(case))
                self.check_reports(result, case)
                self.assertEqual(result[1]['copper_area_m2'], {})
                if name == 'zero':
                    for area in result[1]['opening_area_m2'].values():
                        self.same_area(area, dict(polygons=0., raster=0.))

    def test_vertical_metal_counts_as_copper(self):
        stack = fraction_stack()
        baseline = self.invoke(self.write_stack(stack))
        self.check_reports(baseline, stack)
        stack['layers'][1]['copper']['orientation'] = 'vertical'
        vertical = self.invoke(self.write_stack(stack))
        self.check_reports(vertical, stack)
        self.same_area(vertical[1]['copper_area_m2']['L1'],
                       baseline[1]['copper_area_m2']['L1'])

    def test_opening_polygons_conservation(self):
        if 'CHIPCAE_ENABLE_ECAD:BOOL=ON' not in self.cache:
            self.skipTest('polygon case requires CHIPCAE_ENABLE_ECAD=ON')
        source = self.fixture('copper_polygons.stack.json')
        stack = json.loads(source.read_text(encoding='utf-8'))
        polygons = stack['layers'][0]['copper']['polygons']
        sidecar = self.fixture(polygons['file'])
        dbu, layers = decode_polyset(sidecar)
        rings = layers[polygons['layer']]
        # Python integer shoelace is independent of preview's polygon union.
        twice_area = sum(x * v - u * y for ring in rings
                         for (x, y), (u, v) in zip(ring, ring[1:] + ring[:1]))
        reference = twice_area / 2 * dbu**2
        self.close_area(reference, 2e-6)
        baseline = self.invoke(source, nx=10, ny=8)
        self.check_reports(baseline, stack)
        stack['layers'][0]['copper']['role'] = 'opening'
        source.write_text(json.dumps(stack), encoding='utf-8')
        routed = self.invoke(source, nx=10, ny=8)
        self.check_reports(routed, stack)
        name = stack['layers'][0]['name']
        area = routed[1]['opening_area_m2'][name]
        self.same_area(area, baseline[1]['copper_area_m2'][name])
        for value in area.values():
            self.close_area(value, reference)

    def test_opening_vertical_rejected(self):
        stack = fraction_stack()
        stack['layers'][2]['copper'].update(role='opening', orientation='vertical')
        self.invoke(self.write_stack(stack), success=False)


if __name__ == '__main__':
    started = time.perf_counter()
    try:
        unittest.main(argv=[sys.argv[0]], verbosity=2)
    finally:
        usage = resource.getrusage(resource.RUSAGE_CHILDREN)
        divisor = 1024**2 if sys.platform == 'darwin' else 1024
        print(f'MEASURE wall_s={time.perf_counter() - started:.6f} '
              f'rss_mib={usage.ru_maxrss / divisor:.6f}', flush=True)
