"""P2B acceptance: preview must cut polygon regions, including after void removal."""

import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest

import h5py
import numpy as np


def chamfer_lid(whole_cell_void=False):
    if whole_cell_void:
        box = [0, 0, .02, .02]
        polygon = [[.006, 0], [.014, 0], [.02, .006], [.02, .014],
                   [.014, .02], [.006, .02], [0, .014], [0, .006]]
    else:
        box = [0, 0, .056, .044]
        polygon = [[.002, 0], [.054, 0], [.056, .002], [.056, .042],
                   [.054, .044], [.002, .044], [0, .042], [0, .002]]
    return dict(
        id='chamfer_lid', unit='m', symmetry='none', package_type='FCBGA',
        outline=dict(box=box), layers=[
            dict(name='substrate', thickness=.001, kind='Homogenized',
                 material='organic_eq', n_elem_min=2, grading=1),
            dict(name='lid_layer', thickness=.0005, kind='Explicit',
                 n_elem_min=2, grading=1, regions=[
                     dict(name='lid', body='lid', material='solid',
                          footprint=dict(box=box, polygon=polygon))])])


def demo_gate():
    # Transcribe test_demo_gate_void_digest.cpp::fixture(), including defaults.
    return dict(
        id='fcbga_demo', unit='m', symmetry='none', package_type='FCBGA',
        outline=dict(box=[0, 0, .02, .02], polygon=[
            [.002, 0], [.018, 0], [.02, .002], [.02, .018],
            [.018, .02], [.002, .02], [0, .018], [0, .002]]),
        layers=[
            dict(name='substrate', material='organic_eq', thickness=.001,
                 kind='Homogenized', n_elem_min=2, grading=1),
            dict(name='bump_eq', material='sac305_ball_eq', thickness=.0001,
                 kind='Homogenized', n_elem_min=2, grading=1),
            dict(name='die_layer', material='underfill', thickness=.0005,
                 kind='Explicit', n_elem_min=2, grading=1, regions=[
                     dict(name='die', material='silicon',
                          footprint=dict(box=[.005, .005, .015, .015]))])],
        activation_groups=[dict(name='assembly', members=['layer:substrate', 'body:die'])])


def diagnostics(result):
    return (f'\ncommand: {shlex.join(result.args)}\nexit: {result.returncode}'
            f'\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}')


class PreviewRegionPlanes(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = Path(os.environ['CHIPCAE_BINARY_DIR'])
        cls.preview = cls.build / 'preview' / 'chipcae-preview'

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='preview-region-planes-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)

    def invoke(self, name, stack, n, launcher=()):
        source = self.root / f'{name}.json'
        source.write_text(json.dumps(stack), encoding='utf-8')
        stem = self.root / name
        command = [*launcher, str(self.preview), str(source), '--nx', str(n),
                   '--ny', str(n), '--levels', '1', '--max-ratio', '10', '-o', str(stem)]
        env = dict(os.environ, OMPI_ALLOW_RUN_AS_ROOT='1',
                   OMPI_ALLOW_RUN_AS_ROOT_CONFIRM='1')
        try:
            # At most six invocations, leaving headroom under CTest's 60 s limit.
            result = subprocess.run(command, capture_output=True, text=True, env=env, timeout=8)
        except subprocess.TimeoutExpired as error:
            self.fail(f'command timed out: {shlex.join(command)}'
                      f'\nstdout:\n{error.stdout}\nstderr:\n{error.stderr}')
        try:
            events = [json.loads(line) for line in result.stdout.splitlines() if line.strip()]
        except json.JSONDecodeError as error:
            self.fail(f'invalid stdout JSON: {error}{diagnostics(result)}')
        return result, events, stem

    def success(self, result, events):
        context = diagnostics(result)
        self.assertEqual(result.returncode, 0, context)
        found = [event for event in events if event.get('stage') == 'success']
        self.assertEqual(len(found), 1, context)
        return found[0]

    def check_fields(self, summary, expected, context):
        for field, value in expected.items():
            with self.subTest(field=field):
                self.assertEqual(summary.get(field), value, context)

    def check_file(self, stem, summary, expected_cut, expected_volume, context):
        try:
            with h5py.File(str(stem) + '.vtkhdf', 'r') as file:
                grid = file['VTKHDF']
                points = grid['Points'][:]
                connectivity = grid['Connectivity'][:].reshape(-1, 8)
                corners = points[connectivity]
                cell_volume = np.prod(corners.max(axis=1) - corners.min(axis=1), axis=1)
                fractions = grid['CellData/vol_frac'][:]
                cut_count = int(np.count_nonzero(grid['CellData/cell_kind'][:] == 1))
                volume = float(np.sum(fractions * cell_volume))
        except (OSError, KeyError, ValueError) as error:
            self.fail(f'cannot read preview VTKHDF: {error}{context}')
        relative_error = abs(volume - expected_volume) / expected_volume
        print(f'MEASURED {stem.name}: cells={len(connectivity)} nodes={len(points)} '
              f'cut={cut_count} volume={volume:.17g} relative_error={relative_error:.17g}',
              flush=True)
        for field, actual, expected in (
                ('file cells', len(connectivity), summary['cell_count']),
                ('file nodes', len(points), summary['node_count']),
                ('file cut count', cut_count, expected_cut),
                ('CLI/file cut count', cut_count, summary['cut_cell_count'])):
            with self.subTest(field=field):
                self.assertEqual(actual, expected, context)
        with self.subTest(field='solid volume'):
            self.assertLessEqual(relative_error, 1e-10,
                                 f'volume={volume:.17g}, expected={expected_volume:.17g}{context}')

    def test_region_planes_chamfer_lid(self):
        result, events, stem = self.invoke('chamfer-lid', chamfer_lid(), 4)
        summary = self.success(result, events)
        context = diagnostics(result)
        self.check_fields(summary, dict(
            cell_count=64, node_count=125, cut_cell_count=8,
            mesh_sha256='81c516de6ef3a198571e31e7662b27024c7187df29e26ced386d3dc63204228f'),
            context)
        self.check_file(stem, summary, 8, 3.692e-6, context)

    def test_region_planes_whole_cell_void(self):
        result, events, stem = self.invoke('whole-cell-void', chamfer_lid(True), 8)
        summary = self.success(result, events)
        context = diagnostics(result)
        self.check_fields(summary, dict(
            void_cells=8, cell_count=248, node_count=397, cut_cell_count=40,
            mesh_sha256='b3212d00ab3078c5f721aacce065b6f62b4a10f2dd49058fd8c54dcc7f8829a6'),
            context)
        self.check_file(stem, summary, 40, 5.64e-7, context)

    def test_region_planes_demo_gate_unchanged(self):
        result, events, _ = self.invoke('demo-gate', demo_gate(), 4)
        summary = self.success(result, events)
        self.check_fields(summary, dict(
            cell_count=1728, node_count=2261, cut_cell_count=66,
            mesh_sha256='efc79a629779602538239635706a9f4293e897f8e46b3c2aeb43c22c6b6b9f4e'),
            diagnostics(result))

    def test_region_planes_mpi(self):
        cache = (self.build / 'CMakeCache.txt').read_text(encoding='utf-8')
        mpi = shutil.which('mpiexec') or shutil.which('mpirun')
        if 'CHIPCAE_ENABLE_MPI:BOOL=ON' not in cache or not mpi:
            print('SKIP: region_planes_mpi requires MPI-enabled preview and mpiexec/mpirun',
                  flush=True)
            self.skipTest('MPI-enabled preview and launcher required')
        serial, serial_events, _ = self.invoke('chamfer-lid-np1', chamfer_lid(), 4)
        parallel, parallel_events, _ = self.invoke(
            'chamfer-lid-np2', chamfer_lid(), 4, [mpi, '-n', '2'])
        reference = self.success(serial, serial_events)
        summary = self.success(parallel, parallel_events)
        self.check_fields(summary, {field: reference[field] for field in (
            'mesh_sha256', 'cell_count', 'node_count', 'cut_cell_count')},
            diagnostics(serial) + diagnostics(parallel))

    def test_region_planes_rejects(self):
        stack = chamfer_lid()
        stack['layers'][1]['material'] = 'underfill'
        result, events, stem = self.invoke('material-layer-rejected', stack, 4)
        context = diagnostics(result)
        with self.subTest(field='exit code'):
            self.assertEqual(result.returncode, 13, context)
        failures = [event for event in events if event.get('stage') in ('error', 'failure')]
        with self.subTest(field='error message'):
            self.assertTrue(any('polygon in material layer' in event.get('message', '')
                                for event in failures), context)
        with self.subTest(field='no output'):
            self.assertFalse(Path(str(stem) + '.vtkhdf').exists(), context)


if __name__ == '__main__':
    # chipcae_auto_tests passes the source directory as an unused positional argument.
    unittest.main(argv=[sys.argv[0]], verbosity=2)
