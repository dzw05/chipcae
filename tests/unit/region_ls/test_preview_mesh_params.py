"""Public preview mesh controls: budget, layer minimum and solve alignment."""

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


def synthetic_stack(count=1):
    return dict(id='mesh-params', unit='m', symmetry='none', package_type='FCBGA',
                outline=dict(box=[0, 0, .01, .01]), layers=[
                    dict(name=f'layer-{i}', kind='Explicit', material='silicon',
                         thickness=1e-4, n_elem_min=1, grading=1)
                    for i in range(count)])


def diagnostics(result):
    return (f'\ncommand: {shlex.join(result.args)}\nexit: {result.returncode}'
            f'\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}')


class PreviewMeshParams(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = Path(os.environ['CHIPCAE_BINARY_DIR'])
        cls.cache = {}
        for line in (cls.build / 'CMakeCache.txt').read_text(encoding='utf-8').splitlines():
            if line and not line.startswith(('#', '//')) and '=' in line and ':' in line:
                key, value = line.split('=', 1)
                cls.cache[key.split(':', 1)[0]] = value
        cls.unavailable = None
        for feature in ('CHIPCAE_ENABLE_JSONCONS', 'CHIPCAE_ENABLE_HDF5'):
            if cls.cache.get(feature) == 'OFF':
                cls.unavailable = f'preview requires {feature}=ON'
        cls.preview = cls.build / 'preview' / 'chipcae-preview'
        if not cls.unavailable and not cls.preview.is_file():
            raise AssertionError(f'enabled preview executable is missing: {cls.preview}')

    def setUp(self):
        if self.unavailable:
            self.skipTest(self.unavailable)
        temporary = tempfile.TemporaryDirectory(prefix='preview-mesh-params-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.env = dict(os.environ, OMPI_ALLOW_RUN_AS_ROOT='1',
                        OMPI_ALLOW_RUN_AS_ROOT_CONFIRM='1')
        self.measurements = []

    def run_command(self, command, timeout=6):
        try:
            return subprocess.run(command, capture_output=True, text=True,
                                  env=self.env, timeout=timeout)
        except subprocess.TimeoutExpired as error:
            self.fail(f'command timed out: {shlex.join(command)}'
                      f'\nstdout:\n{error.stdout}\nstderr:\n{error.stderr}')

    def invoke(self, name, stack, flags=(), nx=4, ny=4, levels=1):
        source = self.root / f'{name}.json'
        source.write_text(json.dumps(stack), encoding='utf-8')
        stem = self.root / name
        command = [str(self.preview), str(source), '--nx', str(nx), '--ny', str(ny),
                   '--levels', str(levels), '-o', str(stem), *flags]
        result, events = self.preview_command(command)
        return result, events, stem

    def preview_command(self, command):
        start = time.perf_counter()
        try:
            result = self.run_command(command)
        finally:
            # ru_maxrss is cumulative across terminated children, not this call's peak.
            elapsed = time.perf_counter() - start
            rss = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
            self.measurements.append(dict(
                wall_seconds=elapsed,
                children_maxrss_cumulative_mib=rss / (1024**2 if sys.platform == 'darwin'
                                                     else 1024)))
        try:
            events = [json.loads(line) for line in result.stdout.splitlines() if line.strip()]
        except json.JSONDecodeError as error:
            self.fail(f'invalid stdout JSON: {error}{diagnostics(result)}')
        return result, events

    def measure(self, label):
        print(f'MEASURE {label}: ' + json.dumps(self.measurements), flush=True)

    def success(self, invocation):
        result, events, _ = invocation
        context = diagnostics(result)
        self.assertEqual(result.returncode, 0, context)
        found = [event for event in events if event.get('stage') == 'success']
        self.assertEqual(len(found), 1, context)
        self.assertFalse(any(event.get('stage') == 'error' for event in events), context)
        summary = found[0]
        try:
            message = json.loads(summary['message'])
        except (KeyError, TypeError, json.JSONDecodeError) as error:
            self.fail(f'invalid success.message: {error}{context}')
        self.assertIsInstance(message, dict, context)
        self.assertEqual(set(message), set(summary) - {'stage', 'progress', 'message'}, context)
        for key, value in message.items():
            self.assertEqual(summary[key], value, f'{key}{context}')
        return summary

    def rejected(self, invocation, code, prefix):
        result, events, stem = invocation
        context = diagnostics(result)
        self.assertEqual(result.returncode, code, context)
        errors = [event for event in events if event.get('stage') == 'error']
        self.assertEqual(len(errors), 1, context)
        self.assertTrue(errors[0].get('message', '').startswith(prefix), context)
        self.assertFalse(any(event.get('stage') == 'success' for event in events), context)
        # Both grid and embedded-boundary files share the requested stem.
        self.assertEqual(list(self.root.glob(stem.name + '*.vtkhdf')), [], context)

    def test_success_key_order(self):
        expected = [
            'grid', 'max_ratio', 'n0', 'nx0', 'ny0', 'L',
            'padded_cells', 'void_cells', 'nz', 'node_count',
            'cell_count', 'cut_cell_count', 'mesh_sha256',
            'cell_budget', 'min_elems_per_layer', 'copper_area_m2',
        ]
        cases = (
            ('default', [], None),
            ('explicit', ['--cell-budget', '50000', '--min-elems-per-layer', '2'], 2),
        )
        try:
            for name, flags, minimum in cases:
                with self.subTest(name=name):
                    invocation = self.invoke(f'key-order-{name}', synthetic_stack(), flags,
                                             nx=4, ny=4, levels=1)
                    summary = self.success(invocation)
                    result = invocation[0]
                    context = diagnostics(result)
                    events = [json.loads(line, object_pairs_hook=list)
                              for line in result.stdout.splitlines() if line.strip()]
                    found = [event for event in events if ('stage', 'success') in event]
                    self.assertEqual(len(found), 1, context)
                    event = found[0]
                    messages = [value for key, value in event if key == 'message']
                    self.assertEqual(len(messages), 1, context)
                    message = json.loads(messages[0], object_pairs_hook=list)
                    message_keys = [key for key, _ in message]
                    top_keys = [key for key, _ in event]
                    self.assertEqual(message_keys, expected, context)
                    self.assertEqual(top_keys, ['stage', 'progress', *expected, 'message'],
                                     context)
                    for keys in (message_keys, top_keys):
                        self.assertEqual(keys.index('cell_budget'),
                                         keys.index('mesh_sha256') + 1, context)
                        self.assertEqual(keys.index('min_elems_per_layer'),
                                         keys.index('mesh_sha256') + 2, context)
                    self.assertEqual(message_keys[-3:], expected[-3:], context)
                    self.assertEqual(top_keys[-4:-1], expected[-3:], context)
                    self.assertEqual(message[-1], ('copper_area_m2', []), context)
                    self.assertEqual(summary['copper_area_m2'], {}, context)
                    for budget, actual_minimum in (
                            (message[-3][1], message[-2][1]),
                            (summary['cell_budget'], summary['min_elems_per_layer'])):
                        self.assertEqual(budget, 50000, context)
                        if minimum is None:
                            self.assertIsNone(actual_minimum, context)
                        else:
                            self.assertIs(type(actual_minimum), int, context)
                            self.assertEqual(actual_minimum, minimum, context)
        finally:
            self.measure('success_key_order')

    def test_budget_default_rejects(self):
        self.rejected(self.invoke('default-budget', synthetic_stack(15),
                                  nx=60, ny=60, levels=0), 14, 'cell_budget: ')

    def test_budget_raised_accepts(self):
        try:
            invocation = self.invoke('raised-budget', synthetic_stack(15),
                                     ['--cell-budget', '100000'], nx=60, ny=60, levels=0)
            summary = self.success(invocation)
            context = diagnostics(invocation[0])
            self.assertEqual(summary['nz'], 15, context)
            self.assertGreater(summary['cell_count'], 50000, context)
            self.assertLessEqual(summary['cell_count'], 100000, context)
            self.assertEqual(summary.get('cell_budget'), 100000, context)
            self.assertIn('min_elems_per_layer', summary, context)
            self.assertIsNone(summary['min_elems_per_layer'], context)
        finally:
            self.measure('raised-budget')

    def test_budget_equal_cell_count(self):
        baseline_run = self.invoke('baseline', synthetic_stack())
        baseline = self.success(baseline_run)
        exact_run = self.invoke('exact', synthetic_stack(),
                                ['--cell-budget', str(baseline['cell_count'])])
        exact = self.success(exact_run)
        context = diagnostics(baseline_run[0]) + diagnostics(exact_run[0])
        self.assertEqual(exact['mesh_sha256'], baseline['mesh_sha256'], context)
        self.assertEqual(exact['cell_count'], baseline['cell_count'], context)
        self.assertEqual(exact.get('cell_budget'), baseline['cell_count'], context)
        self.assertIn('min_elems_per_layer', exact, context)
        self.assertIsNone(exact['min_elems_per_layer'], context)
        self.rejected(self.invoke('too-small', synthetic_stack(), ['--cell-budget', '1']),
                      14, 'cell_budget: ')

    def test_min_elems_override(self):
        for minimum, original_minimum, expected_nz in ((1, 1, 15), (1, 3, 17),
                                                       (2, 1, 30), (2, 3, 31)):
            with self.subTest(minimum=minimum, original_minimum=original_minimum):
                stack = synthetic_stack(15)
                stack['layers'][0]['n_elem_min'] = original_minimum
                flags = ['--min-elems-per-layer', str(minimum)]
                # Equal layer thicknesses make this subdivision ratio exceed the default 2.
                if original_minimum > 2 * minimum:
                    flags += ['--max-ratio', '10']
                invocation = self.invoke(f'min-{minimum}-{original_minimum}', stack, flags)
                summary = self.success(invocation)
                self.assertEqual(summary['nz'], expected_nz, diagnostics(invocation[0]))
                self.assertEqual(summary.get('min_elems_per_layer'), minimum,
                                 diagnostics(invocation[0]))

    def test_defaults_unchanged(self):
        # Reuse the stem: success includes the output path, including in message.
        plain_run = self.invoke('same-stem', synthetic_stack())
        plain = self.success(plain_run)
        explicit_run = self.invoke('same-stem', synthetic_stack(), ['--cell-budget', '50000'])
        explicit = self.success(explicit_run)
        context = diagnostics(plain_run[0]) + diagnostics(explicit_run[0])
        self.assertEqual(set(plain), set(explicit), context)
        for key in plain:
            self.assertEqual(plain[key], explicit[key], f'{key}{context}')
        self.assertEqual(plain.get('cell_budget'), 50000, context)
        self.assertIn('min_elems_per_layer', plain, context)
        self.assertIsNone(plain['min_elems_per_layer'], context)

    def test_matches_solve(self):
        solve = self.build / 'apps' / 'chipcae-solve' / 'chipcae-solve'
        if not solve.is_file():
            self.skipTest('solve executable unavailable')
        mpi = shutil.which('mpiexec') or shutil.which('mpirun')
        if not mpi:
            configured = self.cache.get('MPIEXEC_EXECUTABLE', '')
            mpi = configured if configured and os.access(configured, os.X_OK) else None
        if not mpi:
            self.skipTest('MPI launcher unavailable')
        stack = synthetic_stack(2)
        for layer, thickness in zip(stack['layers'], (.1, .15)):
            layer['thickness'] = thickness
        (self.root / 'stack.json').write_text(json.dumps(stack), encoding='utf-8')

        def table(columns):
            return dict(temperatures=[300], columns=columns,
                        interpolation='constant', extrapolation='clamp')

        material = dict(
            id='silicon', schema_version='1', unit_system='SI', model='thermo_elastic',
            density=2330, elastic=dict(symmetry='isotropic',
                                      table=table({'E': [130e9], 'nu': [.3]})),
            thermal_expansion=dict(symmetry='isotropic', cte_type='tangent',
                                   stress_free_temperature=300,
                                   table=table({'alpha': [2.6e-6]})),
            provenance=dict(source_type='illustrative', confidence='illustrative'))
        (self.root / 'materials.json').write_text(json.dumps(
            {'schema': 'chipcae.material_library/1', 'materials': [material]}), encoding='utf-8')
        case = {
            'schema': 'chipcae.case/1.0', 'stack': {'ref': 'stack.json'},
            'materials': {'ref': 'materials.json'},
            'mesh': {'type': 'aligned_cartesian_cut', 'nx': 4, 'ny': 2, 'levels': 1},
            'physics': [{'id': 'mech', 'solver': 'fem.solid'}],
            'loads': [{'type': 'uniform_temperature', 'T_ref': 300, 'T_target': 300}],
            'constraints': {'rigid': '3-2-1'},
            'solver': {'linear': {'preconditioner': 'direct'}},
            'output': {'format': 'vtkhdf'}}
        (self.root / 'case.json').write_text(json.dumps(case), encoding='utf-8')
        profile = {'schema': 'chipcae.run_profile/1.0', 'ranks': 1, 'backend': 'cpu', 'gpus': 0}
        (self.root / 'profile.json').write_text(json.dumps(profile), encoding='utf-8')
        out = self.root / 'solve'
        result = self.run_command(
            [mpi, '-n', '1', str(solve), '--case', str(self.root / 'case.json'),
             '--profile', str(self.root / 'profile.json'), '--out', str(out)], timeout=12)
        if result.returncode == 2 and 'numerical backend unavailable:' in result.stderr:
            self.skipTest('solve explicitly reports numerical backend unavailable')
        self.assertEqual(result.returncode, 0, diagnostics(result))
        kernel = json.loads((out / 'manifest.json').read_text(encoding='utf-8'))['mesh']
        try:
            default_run = self.invoke('preview-default', stack, ny=2)
            default = self.success(default_run)
            context = diagnostics(result) + diagnostics(default_run[0])
            self.assertEqual(default['nz'], 2, context)
            self.assertNotEqual(default['nz'], kernel['nz'], context)
            aligned_run = self.invoke(
                'preview-aligned', stack, ['--max-ratio', '2', '--min-elems-per-layer', '2',
                                           '--cell-budget', '50000'], ny=2)
            aligned = self.success(aligned_run)
            context += diagnostics(aligned_run[0])
            self.assertEqual(aligned['nz'], 4, context)
            for key in ('nx0', 'ny0', 'L', 'nz', 'padded_cells', 'mesh_sha256'):
                self.assertEqual(aligned[key], kernel[key], f'{key}{context}')
        finally:
            self.measure('solve-alignment')

    def test_option_errors(self):
        for option in ('--cell-budget', '--min-elems-per-layer'):
            cases = [
                ([option, '2', option, '2'], f'duplicate option: {option}'),
                ([option, '2', '--max-ratio', '2', option, '2'],
                 f'duplicate option: {option}'),
                ([option], f'missing value for {option}'),
                ([option, ''], f'missing value for {option}'),
            ]
            cases.extend(([option, value], f'invalid integer: {value}')
                         for value in ('0', '-1', '1.5', '2147483648'))
            for index, (flags, prefix) in enumerate(cases):
                with self.subTest(option=option, flags=flags):
                    self.rejected(self.invoke(f'error-{option[2:]}-{index}', synthetic_stack(),
                                              flags), 1, prefix)
        with self.subTest(option='--Cell-Budget'):
            self.rejected(self.invoke('case-sensitive', synthetic_stack(),
                                      ['--Cell-Budget', '2']),
                          1, 'unexpected argument: --Cell-Budget')
        with self.subTest(option='--help'):
            result, _ = self.preview_command([str(self.preview), '--help'])
            self.assertEqual(result.returncode, 0, diagnostics(result))
            for option in ('--cell-budget', '--min-elems-per-layer'):
                self.assertIn(option, result.stdout, diagnostics(result))


if __name__ == '__main__':
    # Auto-registration supplies a source directory, not unittest selectors.
    result = unittest.main(argv=[sys.argv[0]], verbosity=2, exit=False).result
    if not result.wasSuccessful():
        sys.exit(1)
    if result.testsRun > 0 and len(result.skipped) == result.testsRun:
        sys.exit(77)
    sys.exit(0)
