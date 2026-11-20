"""Preview opt-in nonconvex outlines: CLI, retained lattice, volume and identity."""

import itertools
import json
import math
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


OUTLINES = {
    'L': [[0, 0], [.02, 0], [.02, .0137], [.0113, .0137], [.0113, .02], [0, .02]],
    'U': [[0, 0], [.02, 0], [.02, .02], [.0129, .02], [.0129, .0123],
          [.0071, .0123], [.0071, .02], [0, .02]],
    'V': [[0, 0], [.0064, 0], [.0103, .0087], [.0142, 0], [.02, 0],
          [.02, .02], [0, .02]],
    'square': [[0, 0], [.02, 0], [.02, .02], [0, .02]],
    'bow-tie': [[0, 0], [.02, .02], [.02, 0], [0, .02]],
}
FIELDS = ('cell_count', 'node_count', 'cut_cell_count', 'mesh_sha256')
ARRAYS = ('CellData/vol_frac', 'CellData/cell_kind', 'Points', 'Connectivity')


def stack_s(polygon):
    return dict(
        id='ncv', unit='m', symmetry='none', package_type='FCBGA',
        outline=dict(box=[0, 0, .02, .02], polygon=polygon), layers=[
            dict(name='substrate', thickness=.001, kind='Homogenized',
                 material='organic_eq', n_elem_min=2, grading=1)])


def stack_d():
    return dict(
        id='fcbga_demo', unit='m', symmetry='none', package_type='FCBGA',
        outline=dict(box=[0, 0, .02, .02], polygon=[
            [.002, 0], [.018, 0], [.02, .002], [.02, .018],
            [.018, .02], [.002, .02], [0, .018], [0, .002]]), layers=[
            dict(name='substrate', thickness=.001, kind='Homogenized',
                 material='organic_eq', n_elem_min=2, grading=1),
            dict(name='bump_eq', thickness=.0001, kind='Homogenized',
                 material='sac305_ball_eq', n_elem_min=2, grading=1),
            dict(name='die_layer', thickness=.0005, kind='Explicit',
                 material='underfill', n_elem_min=2, grading=1, regions=[
                     dict(name='die', material='silicon',
                          footprint=dict(box=[.005, .005, .015, .015]))])])


def stack_a():
    box = [0, 0, .056, .044]
    return dict(
        id='chamfer_lid', unit='m', symmetry='none', package_type='FCBGA',
        outline=dict(box=box), layers=[
            dict(name='substrate', thickness=.001, kind='Homogenized',
                 material='organic_eq', n_elem_min=2, grading=1),
            dict(name='lid_layer', thickness=.0005, kind='Explicit',
                 n_elem_min=2, grading=1, regions=[
                     dict(name='lid', body='lid', material='solid', footprint=dict(
                         box=box, polygon=[
                             [.002, 0], [.054, 0], [.056, .002], [.056, .042],
                             [.054, .044], [.002, .044], [0, .042], [0, .002]]))])])


def diagnostics(result):
    return (f'\ncommand: {shlex.join(result.args)}\nexit: {result.returncode}'
            f'\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}')


class PreviewOutlineNonconvex(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = Path(os.environ['CHIPCAE_BINARY_DIR'])
        cls.preview = cls.build / 'preview' / 'chipcae-preview'
        cls.timings = {}

    @classmethod
    def tearDownClass(cls):
        for outline in ('L', 'U', 'V'):
            for baseline in ('square-default', 'D-default'):
                if outline in cls.timings and baseline in cls.timings:
                    print(f'MEASURED {outline}/{baseline} wall-time ratio='
                          f'{cls.timings[outline] / cls.timings[baseline]:.6g}', flush=True)

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='preview-outline-nonconvex-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)

    def invoke(self, name, stack, n=8, flags=(), launcher=(), prefix_flags=()):
        source = self.root / f'{name}.json'
        source.write_text(json.dumps(stack), encoding='utf-8')
        stem = self.root / name
        command = [*launcher, str(self.preview), *prefix_flags, str(source), '--nx', str(n),
                   '--ny', str(n), '--levels', '1', '--max-ratio', '10',
                   '-o', str(stem), *flags]
        env = dict(os.environ, OMPI_ALLOW_RUN_AS_ROOT='1',
                   OMPI_ALLOW_RUN_AS_ROOT_CONFIRM='1')
        start = time.perf_counter()
        try:
            # Same per-call limit as P2B; CTest also bounds the whole test at 60 s.
            result = subprocess.run(command, capture_output=True, text=True, env=env, timeout=8)
        except subprocess.TimeoutExpired as error:
            self.fail(f'command timed out: {shlex.join(command)}'
                      f'\nstdout:\n{error.stdout}\nstderr:\n{error.stderr}')
        finally:
            elapsed = time.perf_counter() - start
            rss = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
            rss_mib = rss / (1024 ** 2 if sys.platform == 'darwin' else 1024)
            print(f'MEASURED {name}: wall_seconds={elapsed:.6f} '
                  f'cumulative child-process high-water RSS={rss_mib:.3f} MiB', flush=True)
        if result.returncode == 0:
            self.timings[name] = elapsed
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
        self.assertFalse(any(event.get('stage') in ('error', 'failure') for event in events),
                         context)
        return found[0]

    def read_arrays(self, stem, context):
        try:
            with h5py.File(str(stem) + '.vtkhdf', 'r') as file:
                return {name: file['VTKHDF'][name][:] for name in ARRAYS}
        except (OSError, KeyError, ValueError) as error:
            self.fail(f'cannot read preview VTKHDF: {error}{context}')

    def check_fields(self, summary, expected, context):
        for field, value in expected.items():
            with self.subTest(field=field):
                self.assertEqual(summary.get(field), value, context)

    def check_lattice(self, label, points, connectivity, context):
        # Positive-area intersections derived from the brief's polygons on the 8x8 grid:
        # L loses the upper-right 3x2 cells, U the upper-middle 2x3,
        # V only (4,0). Each xy cell occurs in both z layers, once only.
        expected = {
            (i, j, k) for i, j, k in itertools.product(range(8), range(8), range(2))
            if not ((label == 'L' and i >= 5 and j >= 6)
                    or (label == 'U' and i in (3, 4) and j >= 5)
                    or (label == 'V' and (i, j) == (4, 0)))
        }
        self.assertEqual(len(connectivity), {'L': 116, 'U': 116, 'V': 126}[label], context)
        self.assertEqual(points.shape[1:], (3,), context)
        self.assertTrue(np.all(np.isfinite(points)), context)
        spacing = np.array([.0025, .0025, .0005], dtype=np.float64)
        indices = np.rint(points / spacing).astype(np.int64)
        # Absolute tolerance in metres; much smaller than any lattice spacing.
        np.testing.assert_allclose(points, indices * spacing, rtol=0, atol=1e-12,
                                   err_msg=context)
        self.assertTrue(np.all(connectivity >= 0), context)
        self.assertTrue(np.all(connectivity < len(points)), context)
        corners = indices[connectivity]
        lower, upper = corners.min(axis=1), corners.max(axis=1)
        self.assertTrue(np.all(upper - lower == 1), context)
        actual_cells = [tuple(cell) for cell in lower]
        self.assertEqual(len(set(actual_cells)), len(actual_cells), context)
        self.assertEqual(set(actual_cells), expected, context)
        offsets = list(itertools.product((0, 1), repeat=3))
        for cell, cell_corners in zip(lower, corners):
            self.assertEqual({tuple(point) for point in cell_corners},
                             {tuple(cell + offset) for offset in offsets}, context)
        expected_nodes = {tuple(a + b for a, b in zip(cell, offset))
                          for cell in expected for offset in offsets}
        actual_nodes = [tuple(point) for point in indices]
        self.assertEqual(len(set(actual_nodes)), len(actual_nodes), context)
        self.assertEqual(set(actual_nodes), expected_nodes, context)
        self.assertEqual(set(connectivity.ravel().tolist()), set(range(len(points))), context)

    def test_nonconvex_volumes(self):
        for label, expected_volume in (('L', 3.4519e-7), ('U', 3.5534e-7), ('V', 3.6607e-7)):
            with self.subTest(outline=label):
                result, events, stem = self.invoke(
                    label, stack_s(OUTLINES[label]), flags=['--nonconvex-outline'])
                summary = self.success(result, events)
                context = diagnostics(result)
                arrays = self.read_arrays(stem, context)
                points = arrays['Points'].astype(np.float64)
                fractions = arrays['CellData/vol_frac'].astype(np.float64)
                connectivity = arrays['Connectivity']
                # VTKHDF stores eight integer node references per background hex.
                # Validate the stored layout before reshape can hide a malformed array.
                self.assertEqual(connectivity.ndim, 1, context)
                self.assertTrue(np.issubdtype(connectivity.dtype, np.integer), context)
                self.assertEqual(connectivity.size % 8, 0, context)
                connectivity = connectivity.reshape(-1, 8)
                kinds = arrays['CellData/cell_kind']
                self.assertEqual(fractions.shape, (len(connectivity),), context)
                self.assertEqual(kinds.shape, (len(connectivity),), context)
                self.assertTrue(np.all(np.isfinite(fractions)), context)
                self.assertTrue(np.all((fractions > 0) & (fractions <= 1)), context)
                self.assertTrue(np.issubdtype(kinds.dtype, np.integer), context)
                self.assertTrue(np.all(np.isin(kinds, (0, 1))), context)
                self.check_lattice(label, points, connectivity, context)
                corners = points[connectivity]
                bbox_volume = np.prod(corners.max(axis=1) - corners.min(axis=1), axis=1)
                volume = math.fsum(float(f) * float(v) for f, v in zip(fractions, bbox_volume))
                relative_error = abs(volume - expected_volume) / expected_volume
                print(f'MEASURED {label}: volume={volume:.17g} '
                      f'absolute_error={abs(volume - expected_volume):.17g} '
                      f'relative_error={relative_error:.17g}', flush=True)
                self.assertLessEqual(relative_error, 1e-8, context)
                cut_count = int(np.count_nonzero(kinds == 1))
                self.assertGreater(cut_count, 0, context)
                self.check_fields(summary, dict(cell_count=len(connectivity),
                                               node_count=len(points), cut_cell_count=cut_count),
                                  context)
                self.assertFalse(np.any(kinds == 2), context)
                self.assertTrue(np.all(fractions > 0), context)
                print(f'MEASURED {label}: cells={len(connectivity)} nodes={len(points)} '
                      f'cut={cut_count}; retained lattice verified', flush=True)

    def check_rejected(self, result, events, stem, message, exit_code=13):
        context = diagnostics(result)
        with self.subTest(field='exit code'):
            self.assertEqual(result.returncode, exit_code, context)
        with self.subTest(field='error message'):
            self.assertTrue(any(message in event.get('message', '') for event in events
                                if event.get('stage') in ('error', 'failure')), context)
        with self.subTest(field='no output'):
            self.assertFalse(Path(str(stem) + '.vtkhdf').exists(), context)
        with self.subTest(field='no success event'):
            self.assertFalse(any(event.get('stage') == 'success' for event in events), context)

    def test_default_unchanged(self):
        result, events, stem = self.invoke('L-default', stack_s(OUTLINES['L']))
        self.check_rejected(result, events, stem, 'non-convex outlines are not supported')

    def test_self_intersecting(self):
        result, events, stem = self.invoke(
            'bow-tie', stack_s(OUTLINES['bow-tie']), flags=['--nonconvex-outline'])
        self.check_rejected(result, events, stem, 'self-intersecting outline')

    def test_duplicate_flag(self):
        # Duplicate detection must span the positional input and valued options,
        # as well as catching adjacent copies at the end of the command.
        for name, prefix_flags, flags in (
                ('adjacent', [], ['--nonconvex-outline', '--nonconvex-outline']),
                ('separated', ['--nonconvex-outline'], ['--nonconvex-outline'])):
            with self.subTest(placement=name):
                result, events, stem = self.invoke(
                    'duplicate-' + name, stack_s(OUTLINES['square']),
                    prefix_flags=prefix_flags, flags=flags)
                self.check_rejected(result, events, stem, 'duplicate option: --nonconvex-outline',
                                    exit_code=1)

    def test_convex_bitwise(self):
        for name, stack, n, counts, digest in (
                ('square', stack_s(OUTLINES['square']), 8, (128, 243, 0),
                 '7d19af16125c39b50e2bd548a21401dd517a3acb53dd7d3a2b80eecefd3cc466'),
                ('D', stack_d(), 4, (1728, 2261, 66),
                 'efc79a629779602538239635706a9f4293e897f8e46b3c2aeb43c22c6b6b9f4e'),
                ('A', stack_a(), 4, (64, 125, 8),
                 '81c516de6ef3a198571e31e7662b27024c7187df29e26ced386d3dc63204228f')):
            with self.subTest(stack=name):
                plain, plain_events, plain_stem = self.invoke(name + '-default', stack, n)
                flagged, flagged_events, flagged_stem = self.invoke(
                    name + '-flagged', stack, n, flags=['--nonconvex-outline'])
                reference = self.success(plain, plain_events)
                context = diagnostics(plain) + diagnostics(flagged)
                expected = dict(zip(FIELDS, (*counts, digest)))
                self.check_fields(reference, expected, context)
                summary = self.success(flagged, flagged_events)
                self.check_fields(summary, expected, context)
                self.check_fields(summary, {field: reference[field] for field in FIELDS}, context)
                before = self.read_arrays(plain_stem, context)
                after = self.read_arrays(flagged_stem, context)
                for array_name in ARRAYS:
                    with self.subTest(array=array_name):
                        a, b = before[array_name], after[array_name]
                        self.assertEqual(a.shape, b.shape, context)
                        self.assertEqual(a.dtype, b.dtype, context)
                        np.testing.assert_array_equal(a, b, err_msg=context)
                        if np.issubdtype(a.dtype, np.floating):
                            self.assertEqual(a.tobytes(), b.tobytes(), context)
                print(f'MEASURED {name}: convex summaries and all four arrays bitwise equal',
                      flush=True)

    def test_np2(self):
        cache = (self.build / 'CMakeCache.txt').read_text(encoding='utf-8')
        mpi = shutil.which('mpiexec') or shutil.which('mpirun')
        if 'CHIPCAE_ENABLE_MPI:BOOL=ON' not in cache or not mpi:
            print('SKIP: np2 requires MPI-enabled preview and mpiexec/mpirun', flush=True)
            self.skipTest('MPI-enabled preview and launcher required')
        serial, serial_events, _ = self.invoke(
            'L-np1', stack_s(OUTLINES['L']), flags=['--nonconvex-outline'],
            launcher=[mpi, '-n', '1'])
        parallel, parallel_events, _ = self.invoke(
            'L-np2', stack_s(OUTLINES['L']), flags=['--nonconvex-outline'],
            launcher=[mpi, '-n', '2'])
        reference = self.success(serial, serial_events)
        summary = self.success(parallel, parallel_events)
        self.check_fields(summary, {field: reference[field] for field in FIELDS},
                          diagnostics(serial) + diagnostics(parallel))
        print('MEASURED L: MPI np=1/np=2 success fields exactly equal', flush=True)


if __name__ == '__main__':
    # chipcae_auto_tests passes the source directory as an unused positional argument.
    unittest.main(argv=[sys.argv[0]], verbosity=2)
