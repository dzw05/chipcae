"""CLI acceptance for synthetic notched lids consumed through PieceUnion."""

import json
import os
from pathlib import Path
import resource
import shlex
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

import h5py
import numpy as np


def lid_stack(convex=False):
    box = [0, 0, .056, .044]
    ring = [[.002, 0], [.024, 0], [.026, .010], [.030, .010], [.032, 0],
            [.054, 0], [.056, .002], [.056, .042], [.054, .044], [.002, .044],
            [0, .042], [0, .002]]
    if convex:
        ring = [[.002, 0], [.054, 0], [.056, .002], [.056, .042],
                [.054, .044], [.002, .044], [0, .042], [0, .002]]
    regions = [dict(name='stiffener_' + name, body='stiffener', material='copper',
                    footprint=dict(box=bounds)) for name, bounds in (
        ('south', [.008, .002, .048, .007]), ('north', [.008, .037, .048, .042]),
        ('west', [.008, .007, .013, .037]), ('east', [.043, .007, .048, .037]))]
    return dict(id='synthetic_notched_chamfer_lid', unit='m', symmetry='none',
                package_type='assembly', outline=dict(box=box), layers=[
                    dict(name='substrate', body='substrate', material='organic_eq',
                         kind='Explicit', thickness=.001, n_elem_min=1, grading=1),
                    dict(name='stiffener_layer', kind='Explicit', thickness=.0005,
                         n_elem_min=1, grading=1, regions=regions),
                    dict(name='lid_layer', kind='Explicit', thickness=.0005,
                         n_elem_min=1, grading=1, regions=[dict(
                             name='lid', body='lid', material='copper',
                             footprint=dict(box=box, polygon=ring))])])


def diagnostics(result):
    return (f'\ncommand: {shlex.join(result.args)}\nexit: {result.returncode}'
            f'\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}')


class PreviewLidNonconvex(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = Path(os.environ['CHIPCAE_BINARY_DIR'])
        cls.preview = cls.build / 'preview' / 'chipcae-preview'
        temporary = tempfile.TemporaryDirectory(prefix='preview-lid-nonconvex-')
        cls.addClassCleanup(temporary.cleanup)
        cls.root = Path(temporary.name)
        cls.reference = None

    def setUp(self):
        self.measurements = []

    def tearDown(self):
        print(f'MEASURE {self._testMethodName}: {json.dumps(self.measurements)}', flush=True)

    def invoke(self, label, stack, n, flags=(), launcher=()):
        source = self.root / f'{label}.json'
        source.write_text(json.dumps(stack), encoding='utf-8')
        stem = self.root / label
        command = [*launcher, str(self.preview), str(source), '--nx', str(n),
                   '--ny', str(n), '--levels', '1', *flags, '-o', str(stem)]
        env = dict(os.environ, OMPI_ALLOW_RUN_AS_ROOT='1', OMPI_ALLOW_RUN_AS_ROOT_CONFIRM='1')
        start = time.perf_counter()
        try:
            result = subprocess.run(command, capture_output=True, text=True, env=env, timeout=8)
        except subprocess.TimeoutExpired as error:
            self.fail(f'command timed out: {shlex.join(command)}'
                      f'\nstdout:\n{error.stdout}\nstderr:\n{error.stderr}')
        finally:
            rss = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
            self.measurements.append(dict(
                label=label, seconds=time.perf_counter() - start,
                children_maxrss_cumulative_mib=rss / (1024**2 if sys.platform == 'darwin'
                                                     else 1024)))
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
        self.measurements[-1].update({key: found[0][key] for key in (
            'cell_count', 'node_count', 'cut_cell_count')})
        return found[0]

    def check_file(self, stem, summary, convex, context):
        try:
            with h5py.File(str(stem) + '.vtkhdf', 'r') as file:
                grid = file['VTKHDF']
                points = grid['Points'][:]
                connectivity = grid['Connectivity'][:].reshape(-1, 8)
                bodies = grid['CellData/body'][:].reshape(-1)
                fractions = grid['CellData/vol_frac'][:].reshape(-1)
                kinds = grid['CellData/cell_kind'][:].reshape(-1)
        except (OSError, KeyError, ValueError) as error:
            self.fail(f'cannot read preview VTKHDF: {error}{context}')
        corners = points[connectivity]
        low, high = corners.min(axis=1), corners.max(axis=1)
        # Output hexes are background boxes; vol_frac represents retained volume.
        retained = np.prod(high - low, axis=1) * fractions
        cut_count = int(np.count_nonzero(kinds == 1))
        for actual, expected in ((len(connectivity), summary['cell_count']),
                                 (len(points), summary['node_count']),
                                 (cut_count, summary['cut_cell_count'])):
            self.assertEqual(actual, expected, context)
        self.assertGreater(cut_count, 0, context)
        self.assertEqual(len(np.unique(bodies)), 3, context)
        # Hand-derived SI volumes, independent of mesh resolution and body IDs.
        lid_volume = (.056*.044 - 4*.002*.002/2)*.0005
        if not convex:
            lid_volume -= (.008 + .004)/2*.010*.0005
        expected = dict(substrate=.056*.044*.001,
                        stiffener=(.04*.04 - .03*.03)*.0005, lid=lid_volume)
        volumes, errors = {}, {}
        # Match bodies by their full z extent, never by incidental numeric IDs.
        for body in np.unique(bodies):
            mask = bodies == body
            extent = [low[mask, 2].min(), high[mask, 2].max()]
            names = [name for name, bounds in (
                ('substrate', [0, .001]), ('stiffener', [.001, .0015]),
                ('lid', [.0015, .002])) if np.allclose(extent, bounds, atol=1e-12, rtol=0)]
            self.assertEqual(len(names), 1, f'z extent={extent}{context}')
            name = names[0]
            self.assertNotIn(name, volumes, context)
            volumes[name] = float(retained[mask].sum())
            errors[name] = abs(volumes[name] - expected[name]) / expected[name]
            self.assertLessEqual(errors[name], 1e-10, f'{name}: {volumes}{context}')
            if name == 'lid' and not convex:
                core = (mask & (low[:, 0] >= .026 - 1e-12)
                        & (high[:, 0] <= .030 + 1e-12) & (low[:, 1] >= -1e-12)
                        & (high[:, 1] <= .010 + 1e-12))
                self.assertEqual(int(np.count_nonzero(core)), 0, context)
        self.measurements[-1].update(
            cell_count=len(connectivity), node_count=len(points), cut_cell_count=cut_count,
            body_volumes=volumes, relative_errors=errors)
        return volumes

    def validated(self, label, stack, n, convex=False):
        result, events, stem = self.invoke(label, stack, n)
        summary = self.success(result, events)
        context = diagnostics(result)
        return summary, self.check_file(stem, summary, convex, context), context

    def reference_n4(self):
        if type(self).reference is None:
            type(self).reference = self.validated('N4-reference', lid_stack(), 4)
        return type(self).reference

    def same_summary(self, summary, reference, context):
        for field in ('mesh_sha256', 'cell_count', 'node_count', 'cut_cell_count'):
            self.assertEqual(summary[field], reference[field], f'{field}{context}')

    def test_notched_lid_volumes(self):
        _, coarse, context = self.validated('N2', lid_stack(), 2)
        _, fine, other = self.reference_n4()
        for name in ('substrate', 'stiffener', 'lid'):
            self.assertLessEqual(abs(coarse[name] - fine[name]) / fine[name],
                                 1e-10, f'{name}: N2={coarse}, N4={fine}{context}{other}')

    def test_convex_lid_contrast(self):
        _, notched, context = self.reference_n4()
        _, convex, other = self.validated('C4', lid_stack(convex=True), 4, convex=True)
        for name in ('substrate', 'stiffener'):
            self.assertLessEqual(abs(convex[name] - notched[name]) / notched[name],
                                 1e-12, context + other)
        self.assertLessEqual(abs((convex['lid'] - notched['lid']) - 3e-8) / 3e-8,
                             1e-10, context + other)

    def test_nonconvex_flag_and_repeat(self):
        reference, _, context = self.reference_n4()
        for label, flags in (('N4-repeat', ()), ('N4-general', ('--nonconvex-outline',))):
            result, events, _ = self.invoke(label, lid_stack(), 4, flags)
            self.same_summary(self.success(result, events), reference, context + diagnostics(result))

    def test_ring_orientation_and_closure(self):
        reference, _, context = self.reference_n4()
        for label in ('N4-clockwise', 'N4-closed'):
            stack = lid_stack()
            ring = stack['layers'][2]['regions'][0]['footprint']['polygon']
            if label == 'N4-clockwise':
                ring.reverse()
            else:
                ring.append(ring[0].copy())
            result, events, _ = self.invoke(label, stack, 4)
            self.assertEqual(self.success(result, events)['mesh_sha256'],
                             reference['mesh_sha256'], context + diagnostics(result))

    def test_notched_lid_mpi(self):
        cache = (self.build / 'CMakeCache.txt').read_text(encoding='utf-8')
        mpi = shutil.which('mpiexec') or shutil.which('mpirun')
        if 'CHIPCAE_ENABLE_MPI:BOOL=ON' not in cache or not mpi:
            print('SKIP: notched_lid_mpi requires MPI-enabled preview and mpiexec/mpirun', flush=True)
            self.skipTest('MPI-enabled preview and launcher required')
        reference, _, context = self.reference_n4()
        result, events, _ = self.invoke('N4-np2', lid_stack(), 4, launcher=[mpi, '-n', '2'])
        self.same_summary(self.success(result, events), reference, context + diagnostics(result))

    def test_self_intersecting_lid_rejected(self):
        stack = lid_stack()
        stack['layers'][2]['regions'][0]['footprint']['polygon'] = [
            [0, 0], [.056, .044], [.056, 0], [0, .044]]
        result, events, stem = self.invoke('bowtie', stack, 4)
        context = diagnostics(result)
        self.assertEqual(result.returncode, 13, context)
        self.assertTrue(any(event.get('stage') == 'error'
                            and event.get('message', '').startswith('CUT_UNSUPPORTED')
                            for event in events), context)
        self.assertFalse(Path(str(stem) + '.vtkhdf').exists(), context)


if __name__ == '__main__':
    unittest.main(argv=[sys.argv[0]], verbosity=2)
