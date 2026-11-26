"""I1 CLI acceptance matrix, using public stacks and synthetic material cards.

Requires CHIPCAE_I1_BRANCHPOINT_SOLVE_EXE to name an independently built branch-point solver
to compare every default case: exact stdout, HDF structure and non-solver data,
and global relative L2 error <= 1e-8 for PointData/U. The frozen baseline remains
the reference for legacy stack writing and performance, outside this CLI test.
Baseline builds/configurations are recorded outside the repo.

Thermal-mechanical FVM traverses thermal_problem first. Its CLI rejection does
not independently prove execution of the elastic adapter's guard; the direct C++
guard test covers the helper, and placement in each adapter needs implementation
review. No public test hook is introduced here.
"""

import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import traceback

SOURCE = Path(__file__).resolve().parents[3]
UTIL_PATH = SOURCE / 'tests/integration/fvm_case_e2e/fvm_case_util.py'
SPEC = importlib.util.spec_from_file_location('i1_fvm_case_util', UTIL_PATH)
UTIL = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(UTIL)


def prepare(root, polygons, fraction):
    examples = SOURCE / 'schema/examples'
    name = 'copper_polygons.stack.json' if polygons else 'fcbga_cu.stack.json'
    shutil.copyfile(examples / name, root / 'stack.json')
    if polygons:
        shutil.copyfile(examples / 'copper_polygons.polyset.json',
                        root / 'copper_polygons.polyset.json')
    stack = UTIL.read_json(root / 'stack.json')
    # The public fraction fixture has one legacy, non-copper material ID that
    # cannot name a valid material.v1 card (IDs must be lowercase). Only the
    # temporary solver derivative uses this synthetic lowercase counterpart.
    for layer in stack['layers']:
        if layer.get('material') == 'SAC305_ball_eq':
            layer['material'] = 'sac305_ball_eq'
    target = next(layer for layer in stack['layers'] if 'copper' in layer)
    if not polygons:
        assert target['name'] == 'top_copper'
        target['copper']['fraction'] = fraction
    ids = set()
    for layer in stack['layers']:
        if 'material' in layer:
            ids.add(layer['material'])
        if 'copper' in layer:
            ids.add(layer['copper']['metal'])
        ids.update(region['material'] for region in layer.get('regions', []))
    UTIL.write_json(root / 'materials.json', {
        'schema': 'chipcae.material_library/1',
        'materials': [UTIL.material(name, 390 if name in ('cu', 'copper') else 2)
                      for name in sorted(ids)],
    })
    UTIL.write_json(root / 'profile.json', {
        'schema': 'chipcae.run_profile/1.0', 'backend': 'cpu', 'ranks': 1, 'gpus': 0,
    })
    return stack


def case_for(path):
    case = UTIL.thermal_case('fem' if path == 'fem' else 'fvm')
    if path == 'fvm-thermal':
        case['physics'] = case['physics'][:1]
    return case


def execute(executable, root, name):
    out = root / name
    out.mkdir()
    result = subprocess.run(
        [str(executable), '--case', str(root / 'case.json'),
         '--profile', str(root / 'profile.json'), '--out', str(out)],
        cwd=root, capture_output=True, timeout=90,
    )
    return result, out


def numerical_artifacts(out):
    """Find numerical result files; event logs contain wall-clock times."""
    files = sorted(out.rglob('*.vtkhdf'))
    assert files, f'no numerical result artifacts in {out}'
    return {file.relative_to(out).as_posix(): file for file in files}


def compare_artifacts(actual, expected):
    """Apply the brief's branch-point comparison to every HDF object and U value."""
    import h5py
    import numpy as np

    assert actual.keys() == expected.keys(), 'artifact names differ'
    displacement, reference_displacement = [], []

    def exact_value(x, y, context):
        x, y = np.asarray(x), np.asarray(y)
        assert x.shape == y.shape, f'{context}: shape differs'
        assert x.dtype == y.dtype, f'{context}: dtype differs'
        # Object-valued HDF strings contain Python pointers in tobytes().
        same = np.array_equal(x, y) if x.dtype.hasobject else x.tobytes() == y.tobytes()
        assert same, f'{context}: values differ'

    def attributes(x, y, context):
        assert x.attrs.keys() == y.attrs.keys(), f'{context}: attribute names differ'
        for attribute in x.attrs:
            exact_value(x.attrs[attribute], y.attrs[attribute], f'{context}@{attribute}')

    def datasets(group, ancestors=()):
        # visititems reports each object only once, omitting additional hard links.
        # Enumerate dataset paths independently, including aliases through groups.
        assert group.id not in ancestors, f'{group.name}: cyclic HDF group'
        result = {}
        for key, value in group.items():
            if isinstance(value, h5py.Dataset):
                result[key] = value
            elif isinstance(value, h5py.Group):
                for child, dataset in datasets(value, ancestors + (group.id,)).items():
                    result[f'{key}/{child}'] = dataset
        return result

    for name in actual:
        with h5py.File(actual[name], 'r') as first, h5py.File(expected[name], 'r') as second:
            # File attributes and the complete dataset inventory are checked separately
            # from object traversal, which cannot establish the set of dataset paths.
            attributes(first, second, f'{name}:/')
            actual_datasets, expected_datasets = datasets(first), datasets(second)
            assert actual_datasets.keys() == expected_datasets.keys(), f'{name}: dataset names differ'
            for key, x in actual_datasets.items():
                y = expected_datasets[key]
                context = f'{name}:{key}'
                assert x.shape == y.shape, f'{context}: shape differs'
                assert x.dtype == y.dtype, f'{context}: dtype differs'
                attributes(x, y, context)
                if key.endswith('/PointData/U'):
                    displacement.append(np.asarray(x[...]).ravel())
                    reference_displacement.append(np.asarray(y[...]).ravel())
                else:
                    exact_value(x[...], y[...], context)
            objects, reference_objects = {}, {}
            first.visititems(lambda key, value: objects.update({key: value}))
            second.visititems(lambda key, value: reference_objects.update({key: value}))
            assert objects.keys() == reference_objects.keys(), f'{name}: HDF object names differ'
            for key, x in objects.items():
                y = reference_objects[key]
                context = f'{name}:{key}'
                assert type(x) is type(y), f'{context}: object type differs'
                attributes(x, y, context)
    assert displacement, 'no PointData/U datasets compared'
    x, y = np.concatenate(displacement), np.concatenate(reference_displacement)
    assert np.all(np.isfinite(x)) and np.all(np.isfinite(y)), 'nonfinite displacement'
    error = float(np.linalg.norm(x - y)) / max(float(np.linalg.norm(y)), 1e-300)
    assert error <= 1e-8, f'global U relative difference {error:.17g} > 1e-8'
    return error


def run_case(root, executable, baselines, path, polygons, fraction, variant):
    stack = prepare(root, polygons, fraction)
    copper = next(layer['copper'] for layer in stack['layers'] if 'copper' in layer)
    if variant == 'vertical':
        copper['orientation'] = 'vertical'
    elif variant == 'opening':
        copper['role'] = 'opening'
    UTIL.write_json(root / 'stack.json', stack)
    UTIL.write_json(root / 'case.json', case_for(path))
    result, out = execute(executable, root, 'actual')
    if variant != 'default':
        assert result.returncode == 14, (result.returncode, result.stdout, result.stderr)
        assert b'INPUT_INVALID' in result.stderr, result.stderr
        assert variant.encode() in result.stderr, result.stderr
        return
    assert result.returncode == 0, (result.returncode, result.stdout, result.stderr)
    artifacts = numerical_artifacts(out)
    differences = []
    for label, baseline in baselines:
        reference, reference_out = execute(baseline, root, label)
        try:
            assert reference.returncode == 0, (label, reference.returncode, reference.stderr)
            assert result.stdout == reference.stdout, f'{label}: stdout differs'
            expected = numerical_artifacts(reference_out)
            error = compare_artifacts(artifacts, expected)
            print(f'PASS {label} stdout, HDF structure/data, U relative L2 {error:.17g}',
                  flush=True)
        except AssertionError as error:
            differences.append(f'{label}: {error}')
    assert not differences, differences


def main():
    executable = Path(os.environ['CHIPCAE_SOLVE_EXE']).resolve(strict=True)
    baselines = []
    if os.environ.get('CHIPCAE_I1_BASELINE_SOLVE_EXE'):
        print('CHIPCAE_I1_BASELINE_SOLVE_EXE is not used for solver comparisons; '
              'the revised brief uses only the branch-point solver', flush=True)
    for label, variable in (('branchpoint-baseline', 'CHIPCAE_I1_BRANCHPOINT_SOLVE_EXE'),):
        if os.environ.get(variable):
            baseline = Path(os.environ[variable]).resolve(strict=True)
            assert not baseline.samefile(executable), f'{variable} must be independently built'
            baselines.append((label, baseline))
        else:
            print(f'Baseline comparison not configured: set {variable}', flush=True)
            raise AssertionError(f'{variable} is required for every default-case comparison')
    failures = []
    count = 0
    with tempfile.TemporaryDirectory(prefix='chipcae-i1-solve-') as temp:
        for path in ('fvm-thermal', 'fvm-thermal-mechanical', 'fem'):
            for polygons, fraction in ((False, 0), (False, 0.5), (False, 1), (True, None)):
                for variant in ('default', 'vertical', 'opening'):
                    name = f'{path}-{"polygons" if polygons else fraction}-{variant}'
                    root = Path(temp) / name
                    count += 1
                    try:
                        root.mkdir()
                        run_case(root, executable, baselines, path, polygons, fraction, variant)
                        print(f'PASS {name}', flush=True)
                    except Exception:
                        failures.append(name)
                        print(f'FAIL {name}\n{traceback.format_exc()}', flush=True)
    print(f'{count} matrix cases: {count - len(failures)} passed, {len(failures)} failed', flush=True)
    assert not failures, failures


def self_test():
    """Exercise acceptance-runner failures without requiring solver backends."""
    from contextlib import redirect_stdout
    import io
    import unittest
    from unittest.mock import Mock, patch

    class RunnerTests(unittest.TestCase):
        def setUp(self):
            self.environment = {
                'CHIPCAE_SOLVE_EXE': str(Path(__file__).resolve()),
                'CHIPCAE_I1_BRANCHPOINT_SOLVE_EXE': str(UTIL_PATH),
            }
            self.output = io.StringIO()

        def test_missing_branchpoint_fails_before_running_cases(self):
            del self.environment['CHIPCAE_I1_BRANCHPOINT_SOLVE_EXE']
            run = Mock()
            with patch.dict(os.environ, self.environment, clear=True), \
                    patch.dict(globals(), run_case=run), redirect_stdout(self.output):
                with self.assertRaisesRegex(AssertionError, 'CHIPCAE_I1_BRANCHPOINT_SOLVE_EXE'):
                    main()
            run.assert_not_called()

        def test_unexpected_case_exception_is_reported_and_matrix_continues(self):
            run = Mock(side_effect=[KeyError('synthetic missing material')] + [None] * 35)
            with patch.dict(os.environ, self.environment, clear=True), \
                    patch.dict(globals(), run_case=run), redirect_stdout(self.output):
                with self.assertRaises(AssertionError):
                    main()
            self.assertEqual(run.call_count, 36)
            self.assertIn('KeyError: \'synthetic missing material\'', self.output.getvalue())
            self.assertIn('FAIL fvm-thermal-0-default', self.output.getvalue())
            self.assertIn('PASS fem-polygons-opening', self.output.getvalue())
            self.assertIn('36 matrix cases: 35 passed, 1 failed', self.output.getvalue())

        def test_all_cases_receive_independent_branchpoint(self):
            run = Mock()
            with patch.dict(os.environ, self.environment, clear=True), \
                    patch.dict(globals(), run_case=run), redirect_stdout(self.output):
                main()
            self.assertEqual(run.call_count, 36)
            expected = {
                (path, polygons, fraction, variant)
                for path in ('fvm-thermal', 'fvm-thermal-mechanical', 'fem')
                for polygons, fraction in ((False, 0), (False, 0.5), (False, 1), (True, None))
                for variant in ('default', 'vertical', 'opening')
            }
            self.assertEqual({call.args[3:] for call in run.call_args_list}, expected)
            for call in run.call_args_list:
                self.assertEqual(call.args[2], [('branchpoint-baseline', UTIL_PATH.resolve())])
            self.assertEqual(sum(call.args[-1] == 'default' for call in run.call_args_list), 12)
            self.assertIn('36 matrix cases: 36 passed, 0 failed', self.output.getvalue())

    class ArtifactTests(unittest.TestCase):
        def setUp(self):
            import h5py
            import numpy as np

            temp = tempfile.TemporaryDirectory(prefix='chipcae-i1-artifacts-')
            self.addCleanup(temp.cleanup)
            self.actual = Path(temp.name) / 'actual.vtkhdf'
            self.expected = Path(temp.name) / 'expected.vtkhdf'
            for path in (self.actual, self.expected):
                with h5py.File(path, 'w') as file:
                    file.attrs['version'] = np.array([1, 0], dtype='int32')
                    group = file.create_group('VTKHDF/hex')
                    group.attrs['kind'] = 'synthetic'
                    group.create_dataset('Points', data=np.array([[0., 1., 2.]]))
                    field = group.create_dataset('PointData/U', data=np.array([[1., 2., 3.]]))
                    field.attrs['units'] = 'm'

        def compare(self):
            return compare_artifacts({'result.vtkhdf': self.actual},
                                     {'result.vtkhdf': self.expected})

        def test_exact_and_small_displacement_differences_pass(self):
            import h5py

            self.assertLessEqual(self.compare(), 1e-8)
            with h5py.File(self.actual, 'r+') as file:
                file['VTKHDF/hex/PointData/U'][0, 0] += 1e-9
            self.assertLessEqual(self.compare(), 1e-8)

        def test_dataset_alias_names_must_match(self):
            import h5py

            # visititems visits the Points object once and misses its extra hard link.
            with h5py.File(self.actual, 'r+') as file:
                file['VTKHDF/hex/ZPointsAlias'] = file['VTKHDF/hex/Points']
            with self.assertRaisesRegex(AssertionError, 'dataset names differ'):
                self.compare()

        def test_file_group_and_dataset_attributes_must_match(self):
            import h5py

            for name, attribute, value in (('/', 'version', [1, 1]),
                                           ('VTKHDF/hex', 'kind', 'synthetiX'),
                                           ('VTKHDF/hex/PointData/U', 'units', 's')):
                with self.subTest(name=name):
                    shutil.copyfile(self.expected, self.actual)
                    with h5py.File(self.actual, 'r+') as file:
                        file[name].attrs.modify(attribute, value)
                    with self.assertRaisesRegex(AssertionError, 'values differ'):
                        self.compare()

        def test_dataset_shape_dtype_and_values_must_match(self):
            import h5py
            import numpy as np

            for data, message in ((np.array([1., 2., 3.]), 'shape differs'),
                                  (np.array([[1., 2., 3.]], dtype='float32'), 'dtype differs'),
                                  (np.array([[1., 2., 4.]]), 'relative difference')):
                with self.subTest(message=message):
                    shutil.copyfile(self.expected, self.actual)
                    with h5py.File(self.actual, 'r+') as file:
                        del file['VTKHDF/hex/PointData/U']
                        field = file.create_dataset('VTKHDF/hex/PointData/U', data=data)
                        field.attrs['units'] = 'm'
                    with self.assertRaisesRegex(AssertionError, message):
                        self.compare()
            shutil.copyfile(self.expected, self.actual)
            with h5py.File(self.actual, 'r+') as file:
                file['VTKHDF/hex/Points'][0, 0] = 1e-12
            with self.assertRaisesRegex(AssertionError, 'values differ'):
                self.compare()

    suite = unittest.defaultTestLoader.loadTestsFromTestCase(RunnerTests)
    suite.addTests(unittest.defaultTestLoader.loadTestsFromTestCase(ArtifactTests))
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == '__main__':
    if sys.argv[1:] == ['--self-test']:
        sys.exit(self_test())
    main()
