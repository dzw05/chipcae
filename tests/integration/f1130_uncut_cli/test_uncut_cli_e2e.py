"""IPC → uncut CLI acceptance and strict assertion/verdict regressions."""

from copy import deepcopy
from contextlib import redirect_stderr, redirect_stdout
from datetime import datetime
from io import StringIO
import json
import math
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

SOURCE = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(SOURCE))

TEMPERATURE_ATOL = 1e-8
READBACK_RTOL = 1e-12
PARALLEL_RTOL = 1e-8
QOIS = ('warpage_pv', 'U_mag_max', 'T_max')
ACCEPTANCE_CHECKS = ('import', 'cell_fractions', 'conductivity', 'physics', 'preview',
                     'solve_readback', 'inputs_unchanged', 'numerical', 'parallel')


def write_json(path, obj):
    path.write_text(json.dumps(obj, indent=2, ensure_ascii=False, allow_nan=False) + '\n',
                    encoding='utf-8')


def artifact(path):
    assert path.is_file() and path.stat().st_size > 0, f'missing/empty artifact: {path}'
    return path


def read_json(path):
    return json.loads(artifact(path).read_text(encoding='utf-8'))


def run_command(stage, args, root, timings):
    args = [str(arg) for arg in args]
    started = time.monotonic()
    print(f'COMMAND {stage}: {shlex.join(args)}', flush=True)
    try:
        result = subprocess.run(args, cwd=root, shell=False, capture_output=True,
                                text=True, timeout=600)
    except subprocess.TimeoutExpired as exc:
        print(f'FAIL command={shlex.join(args)} exit=timeout\n'
              f'stdout:\n{exc.stdout or ""}\nstderr:\n{exc.stderr or ""}', flush=True)
        raise AssertionError(f'{stage}: timeout after 600 seconds') from exc
    except OSError as exc:
        print(f'FAIL command={shlex.join(args)} exit=not-started\n'
              f'stdout:\n\nstderr:\n{exc}', flush=True)
        raise AssertionError(f'{stage}: configured command cannot run: {exc}') from exc
    finally:
        timings[stage] = time.monotonic() - started
        print(f'实测 {stage}: {timings[stage]:.6f} s', flush=True)
    if result.returncode != 0:
        print(f'FAIL command={shlex.join(args)} exit={result.returncode}\n'
              f'stdout:\n{result.stdout}\nstderr:\n{result.stderr}', flush=True)
    assert result.returncode == 0, f'{stage}: exit {result.returncode}'
    return result.stdout


def compare_import(imported, golden):
    actual, expected = imported['layers'], golden['layers']
    assert len(actual) == len(expected), 'M1 layer count'
    for index, (got, want) in enumerate(zip(actual, expected, strict=True)):
        context = f'M1 layer[{index}] {got.get("name")!r}'
        for key in ('name', 'material', 'kind'):
            assert got[key] == want[key], f'{context} {key}: {got[key]!r} != {want[key]!r}'
        assert math.isclose(got['thickness'], want['thickness'], rel_tol=1e-12, abs_tol=0), context
        if got['kind'] == 'Patterned':
            assert got['material'] == 'fr4_eq', f'{context} matrix'
            copper = got['copper']
            assert copper['metal'] == 'cu', f'{context} copper metal'
            assert 'polygons' in copper and 'fraction' not in copper, f'{context} polygons required'
            assert copper['polygons']['layer'] == got['name'], f'{context} polygon layer'


def write_solver_inputs(root):
    # Illustrative definitions and case fields copied from test_ipc_uncut_chain.py.
    materials = []
    for name, E, nu, alpha, k in [('cu', 110e9, .34, 17e-6, 390),
                                 ('fr4_eq', 25e9, .3, 1e-5, .3)]:
        def table(columns):
            return dict(temperatures=[298.15],
                        columns={key: [value] for key, value in columns.items()},
                        interpolation='constant', extrapolation='clamp')
        materials.append(dict(
            id=name, schema_version='1', unit_system='SI', model='thermo_elastic',
            density=2000, conductivity=k,
            elastic=dict(symmetry='isotropic', table=table(dict(E=E, nu=nu))),
            thermal_expansion=dict(symmetry='isotropic', cte_type='tangent',
                                   stress_free_temperature=298.15, table=table(dict(alpha=alpha))),
            provenance=dict(source_type='illustrative', confidence='illustrative')))
    write_json(root / 'materials.json', dict(schema='chipcae.material_library/1', materials=materials))
    write_json(root / 'case.json', dict(
        schema='chipcae.case/1.0', stack=dict(ref='stack.json'),
        materials=dict(ref='materials.json'),
        mesh=dict(type='aligned_cartesian_cut', nx=4, ny=4, levels=0, max_ratio=100,
                  min_elems_per_layer=1),
        physics=[dict(id='thermal', solver='fem.conduction', mode='steady'),
                 dict(id='mech', solver='fem.solid', temp_from='thermal', T_ref=298.15)],
        loads=[dict(type='fixed_temperature', target='face:zmin', T=298.15),
               dict(type='convection', target='face:zmax', h=1000, T_inf=348.15)],
        constraints=dict(rigid='3-2-1'), solver=dict(linear=dict(preconditioner='direct', rtol=1e-13)),
        output=dict(format='vtkhdf')))
    for ranks in (1, 2):
        write_json(root / f'profile_r{ranks}.json', dict(
            schema='chipcae.run_profile/1.0', backend='cpu', ranks=ranks, gpus=0))
    p1, p2 = (read_json(root / f'profile_r{ranks}.json') for ranks in (1, 2))
    assert p1.pop('ranks') == 1 and p2.pop('ranks') == 2 and p1 == p2


def inspect_run(run_dir, stack, rings, preview_count, measurements, np, h5py):
    import trace_map_ref as ref

    manifest = read_json(run_dir / 'manifest.json')
    assert manifest['schema_version'] == 'chipcae.results/1.0', manifest.get('schema_version')
    summary = {entry['name']: entry['value'] for entry in manifest['summary']}
    for key in QOIS:
        assert math.isfinite(summary[key]), (key, summary)
        if key != 'T_max':
            assert summary[key] > 0, (key, summary)
    measurements['qoi'] = {key: summary[key] for key in QOIS}
    with h5py.File(artifact(run_dir / 'out/step-0.vtkhdf'), 'r') as file:
        cut_count = int(np.sum(file['VTKHDF/cut/NumberOfCells'][:]))
        hexes = file['VTKHDF/hex']
        count = int(np.sum(hexes['NumberOfCells'][:]))
        measurements.update(cut_cells=cut_count, hex_cells=count, preview_cells=preview_count)
        assert cut_count == 0, f'M3 cut cells={cut_count}'
        assert count > 0 and count == preview_count, f'M2/M3 hex cells={count}, preview={preview_count}'
        mesh = ref.read_hex_mesh(hexes, stack, require_fields=False)
    ref.check_solution(mesh, rings, stack, measurements,
                       expected_areas=ref.COPPER_AREAS_MM2)
    warpage = read_json(run_dir / 'summary.json')['warpage']
    print(f'实测 {run_dir.name} w_bfp_m={warpage["w_bfp_m"]!r}', flush=True)
    readback = warpage['manifest_warpage_pv_m']
    assert readback is not None and math.isfinite(readback), f'M5 readback warpage={readback!r}'
    relative = abs(readback-summary['warpage_pv'])/abs(summary['warpage_pv'])
    measurements.update(readback_warpage_m=readback, readback_relative_error=relative)
    print(f'实测 {run_dir.name}: {json.dumps(measurements, ensure_ascii=False)}', flush=True)
    assert relative <= READBACK_RTOL, f'M3/M5 warpage readback relative error={relative}'
    measurements['acceptance_checks']['numerical'] = True
    return mesh


def get_commit(notes):
    candidate = os.environ.get('CHIPCAE_GIT_SHA', '')
    if re.fullmatch(r'[0-9a-f]{7,40}', candidate):
        return candidate
    try:
        candidate = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=SOURCE,
                                            text=True, stderr=subprocess.PIPE, timeout=10).strip()
        if re.fullmatch(r'[0-9a-f]{7,40}', candidate):
            return candidate
    except (OSError, subprocess.SubprocessError):
        pass
    notes.append('Commit unavailable from valid CHIPCAE_GIT_SHA or git rev-parse HEAD; using 0000000.')
    return '0000000'


def write_result(run_dir, ranks, measurements, timings, notes, failures):
    from tools.vv.check_result_data_class import check_file

    notes = list(notes)
    failures = list(failures)
    checks = measurements.get('acceptance_checks', {})
    failures += [f'acceptance check incomplete: {name}' for name in ACCEPTANCE_CHECKS
                 if checks.get(name) is not True]
    commit = get_commit(notes)
    notes += [f'实测 {stage}: {seconds:.6f} s' for stage, seconds in timings.items()]
    notes += failures
    result = dict(
        schema='chipcae.vv_result/0.2', case_id='f1130_uncut_board4',
        data_class='synthetic', label='实测', date=datetime.now().strftime('%Y%m%d'),
        commit=commit, verdict='fail' if failures else 'pass',
        reference=dict(value=None, type='独立代码', source='trace_map_ref.py: exact clipping + independent Q2'),
        qoi=measurements.get('qoi', {}), run={**measurements, 'ranks': ranks},
        error=dict(value=measurements.get('max_temperature_error_K'), metric='max_abs_temperature_K'),
        tolerance=dict(value=TEMPERATURE_ATOL, metric='max_abs_temperature_K',
                       readback_relative=READBACK_RTOL, parallel_relative=PARALLEL_RTOL),
        timing=timings, notes=notes)
    path = run_dir / 'result.json'
    write_json(path, result)
    # Print before cleanup even when the gate itself fails.
    print(f'RESULT {run_dir.name}/result.json\n{path.read_text(encoding="utf-8")}', flush=True)
    violations = check_file(path)
    assert violations == [], f'result data_class gate: {violations}'
    return failures


def main():
    importer = os.environ.get('CHIPCAE_IMPORT_EXE')
    previewer = os.environ.get('CHIPCAE_PREVIEW_EXE')
    solver = os.environ.get('CHIPCAE_SOLVE_EXE')
    if not importer or not previewer or not solver:
        print('SKIP: importer/preview/solver executable environment unavailable')
        return 77
    try:
        import numpy as np
        import h5py
    except ImportError as exc:
        print(f'SKIP: numpy/h5py unavailable: {exc}')
        return 77

    started = time.monotonic()
    timings, notes, failures = {}, [], []
    with tempfile.TemporaryDirectory(prefix='f1130-uncut-') as directory:
        root = Path(directory)
        # Keep an auditable failure record even if import/preview blocks solving.
        run1 = root / 'ranks1'
        run1.mkdir()
        runs = {1: dict(path=run1, measurements=dict(acceptance_checks={}),
                       completed=False, failures=[])}
        setup_checks = runs[1]['measurements']['acceptance_checks']
        try:
            launcher = shutil.which('mpiexec') or shutil.which('mpirun')
            assert launcher, 'ranks=2: launcher not found (mpiexec/mpirun); acceptance requires MPI'
            material_map = Path(__file__).resolve().with_name('material_map.json')
            run_command('import', [importer, SOURCE / 'tests/integration/ecad/data/board_4layer.xml',
                                  '-o', 'stack.json', '--polyset', 'stack.polyset',
                                  '--report', 'defects.json', '--material-map', material_map],
                        root, timings)
            artifact(root / 'stack.polyset')
            artifact(root / 'defects.json')
            imported = read_json(root / 'stack.json')
            golden = read_json(SOURCE / 'tests/integration/import_cli/golden/board_4layer/stack.json')
            compare_import(imported, golden)
            setup_checks['import'] = True
            import trace_map_ref as ref
            stack = imported
            rings = ref.read_polyset(root / 'stack.polyset')
            # Diagnostics must run even before the missing M3 product fields can gate.
            ref.diagnostics(ref.tensor_mesh(stack, 4, 4), rings, stack)
            stdout = run_command('preview', [previewer, root / 'stack.json', '-o', 'preview',
                                            '--nx', '4', '--ny', '4', '--levels', '0',
                                            '--max-ratio', '100'], root, timings)
            artifact(root / 'preview.vtkhdf')
            events = [json.loads(line) for line in stdout.splitlines() if line.strip()]
            success = [event for event in events if event.get('stage') == 'success']
            assert success, f'M2 missing preview success event: {stdout}'
            preview_count = success[-1].get('cell_count')
            assert type(preview_count) is int and preview_count > 0, success[-1]
            print(f'实测 preview cell_count={preview_count}', flush=True)
            setup_checks['preview'] = True
            write_solver_inputs(root)
            shared = {name: (root / name).read_bytes() for name in
                      ('stack.json', 'stack.polyset', 'case.json', 'materials.json',
                       'profile_r1.json', 'profile_r2.json')}
            run2 = root / 'ranks2'
            run2.mkdir()
            runs[2] = dict(path=run2, measurements=dict(
                acceptance_checks=dict(setup_checks)), completed=False, failures=[])
            for ranks, run in runs.items():
                args = [solver, '--case', 'case.json', '--profile', f'profile_r{ranks}.json',
                        '--out', run['path']]
                if ranks == 2:
                    args = [launcher, '-n', '2'] + args
                try:
                    run_command(f'solve ranks={ranks}', args, root, timings)
                    artifact(run['path'] / 'manifest.json')
                    artifact(run['path'] / 'out/step-0.vtkhdf')
                    run_command(f'readback ranks={ranks}',
                                [sys.executable, SOURCE / 'tools/vv/result_summary.py',
                                 run['path'], '-o', run['path'] / 'summary.json'], root, timings)
                    artifact(run['path'] / 'summary.json')
                    run['completed'] = True
                    run['measurements']['acceptance_checks']['solve_readback'] = True
                    print(f'COMPLETED solve/readback ranks={ranks}', flush=True)
                except (AssertionError, OSError, ValueError, KeyError) as exc:
                    run['failures'].append(f'ranks={ranks}: {exc}')
                finally:
                    try:
                        for name, content in shared.items():
                            assert (root / name).read_bytes() == content, f'shared input changed: {name}'
                        run['measurements']['acceptance_checks']['inputs_unchanged'] = True
                    except (AssertionError, OSError) as exc:
                        run['failures'].append(f'ranks={ranks}: {exc}')
            for ranks, run in runs.items():
                if run['completed']:
                    try:
                        run['mesh'] = inspect_run(run['path'], stack, rings, preview_count,
                                                  run['measurements'], np, h5py)
                    except (AssertionError, OSError, ValueError, KeyError) as exc:
                        run['failures'].append(f'ranks={ranks}: {exc}')
            if len(runs) == 2 and all('qoi' in run['measurements'] for run in runs.values()):
                differences = {}
                parallel_failures = []
                for key in QOIS:
                    serial = runs[1]['measurements']['qoi'][key]
                    parallel = runs[2]['measurements']['qoi'][key]
                    # A zero serial value cannot define the required relative error.
                    assert serial != 0, f'parallel comparison denominator {key}=0'
                    relative = abs(parallel - serial) / abs(serial)
                    differences[key] = relative
                    if relative > PARALLEL_RTOL:
                        parallel_failures.append(
                            f'M3 parallel {key} relative error={relative} > {PARALLEL_RTOL}')
                if all('mesh' in run for run in runs.values()):
                    try:
                        ref.compare_aligned(runs[1]['mesh'], runs[2]['mesh'], PARALLEL_RTOL)
                    except AssertionError as exc:
                        parallel_failures.append(str(exc))
                else:
                    parallel_failures.append('parallel aligned cell/vertex fields unavailable')
                for run in runs.values():
                    run['measurements']['parallel_relative_errors'] = differences
                    run['failures'].extend(parallel_failures)
                    if not parallel_failures:
                        run['measurements']['acceptance_checks']['parallel'] = True
        except (AssertionError, OSError, ValueError, KeyError, TypeError) as exc:
            failures.append(str(exc))
        finally:
            timings['total'] = time.monotonic() - started
            print(f'实测 total: {timings["total"]:.6f} s', flush=True)
            setup_failures = list(failures)
            for ranks, run in runs.items():
                run_notes = notes + [f'solve/readback completed: {run["completed"]}']
                try:
                    run_failures = write_result(run['path'], ranks, run['measurements'], timings,
                                                run_notes, setup_failures + run['failures'])
                    failures.extend(f'ranks={ranks}: {failure}' for failure in run_failures)
                except (AssertionError, OSError, ValueError) as exc:
                    failures.append(f'ranks={ranks} result record: {exc}')
        for failure in failures:
            print(f'FAIL: {failure}', file=sys.stderr, flush=True)
        if failures:
            raise AssertionError('\n'.join(failures))
    return 0


class AcceptanceRecords(unittest.TestCase):
    def test_solver_inputs_use_preview_minimum_and_equivalent_fr4(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            write_solver_inputs(root)
            case = read_json(root / 'case.json')
            self.assertEqual(case['mesh'], dict(type='aligned_cartesian_cut', nx=4, ny=4,
                                                levels=0, max_ratio=100, min_elems_per_layer=1))
            materials = read_json(root / 'materials.json')['materials']
            self.assertEqual([material['id'] for material in materials], ['cu', 'fr4_eq'])
            self.assertEqual(case['stack'], dict(ref='stack.json'))
            self.assertLessEqual(abs(case['solver']['linear']['rtol']-1e-13), 1e-28)
            self.assertEqual(case['loads'], [dict(type='fixed_temperature',target='face:zmin',T=298.15),
                dict(type='convection',target='face:zmax',h=1000,T_inf=348.15)])

    def test_import_compares_golden_directly_and_rejects_wrong_material(self):
        golden = read_json(SOURCE / 'tests/integration/import_cli/golden/board_4layer/stack.json')
        original = json.dumps(golden)
        imported = deepcopy(golden)
        compare_import(imported, golden)
        imported['layers'][0]['material'] = 'cu'
        with self.assertRaisesRegex(AssertionError, 'material:'):
            compare_import(imported, golden)
        self.assertEqual(json.dumps(golden), original)

    def test_import_requires_patterned_base_metal_polygons_and_exact_layer_case(self):
        golden = read_json(SOURCE / 'tests/integration/import_cli/golden/board_4layer/stack.json')
        for defect in ('matrix', 'metal', 'fraction', 'polygons', 'case', 'missing', 'duplicate'):
            imported = deepcopy(golden)
            layer = imported['layers'][1]
            if defect == 'matrix':
                layer['material'] = 'cu'
            elif defect == 'metal':
                layer['copper']['metal'] = 'fr4_eq'
            elif defect == 'fraction':
                layer['copper']['fraction'] = 0
            elif defect == 'polygons':
                layer['copper'].pop('polygons')
            elif defect == 'case':
                layer['name'] = 'in1.cu'
            elif defect == 'missing':
                imported['layers'].pop(1)
            else:
                imported['layers'].insert(1, deepcopy(layer))
            with self.subTest(defect=defect), self.assertRaises(AssertionError):
                compare_import(imported, golden)

    def write_record(self, root, checks, failures=(), ranks=1):
        measurements = dict(
            acceptance_checks=checks,
            qoi=dict(warpage_pv=1e-6, U_mag_max=2e-6, T_max=318.15),
            max_temperature_error_K=1e-10)
        with redirect_stdout(StringIO()):
            write_result(root, ranks, measurements, {}, [], list(failures))
        return json.loads((root / 'result.json').read_text(encoding='utf-8'))

    def test_each_missing_acceptance_check_prevents_pass(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for missing in ACCEPTANCE_CHECKS:
                with self.subTest(missing=missing):
                    checks = {name: True for name in ACCEPTANCE_CHECKS if name != missing}
                    result = self.write_record(root, checks)
                    self.assertEqual(result['verdict'], 'fail')
                    self.assertTrue(any(missing in note for note in result['notes']))

    def test_empty_numerical_record_cannot_pass(self):
        with tempfile.TemporaryDirectory() as directory, redirect_stdout(StringIO()):
            root = Path(directory)
            write_result(root, 1, {}, {}, [], [])
            result = json.loads((root / 'result.json').read_text(encoding='utf-8'))
            self.assertEqual(result['verdict'], 'fail')

    def test_serial_record_without_parallel_comparison_cannot_pass(self):
        with tempfile.TemporaryDirectory() as directory:
            checks = dict.fromkeys(ACCEPTANCE_CHECKS, True)
            checks.pop('parallel', None)
            result = self.write_record(Path(directory), checks)
            self.assertEqual(result['verdict'], 'fail')
            self.assertIn('acceptance check incomplete: parallel', result['notes'])

    def test_missing_launcher_fails_main_and_result_record(self):
        try:
            import numpy  # noqa: F401
            import h5py  # noqa: F401
        except ImportError:
            self.skipTest('main requires numpy/h5py')
        output = StringIO()
        env = dict(CHIPCAE_IMPORT_EXE='unused-importer', CHIPCAE_PREVIEW_EXE='unused-previewer',
                   CHIPCAE_SOLVE_EXE='unused-solver')
        # Launcher discovery is an external dependency; keep verdict generation real.
        with patch.dict(os.environ, env), patch.object(shutil, 'which', return_value=None), \
                redirect_stdout(output), redirect_stderr(StringIO()):
            with self.assertRaisesRegex(AssertionError, 'launcher not found'):
                main()
        record = json.loads(output.getvalue().split('RESULT ranks1/result.json\n', 1)[1])
        self.assertEqual(record['verdict'], 'fail')
        self.assertIn('acceptance check incomplete: parallel', record['notes'])

    def test_parallel_comparison_is_required_for_both_available_runs(self):
        with tempfile.TemporaryDirectory() as directory:
            checks = dict.fromkeys(ACCEPTANCE_CHECKS, True)
            checks.pop('parallel')
            for ranks in (1, 2):
                with self.subTest(ranks=ranks):
                    result = self.write_record(Path(directory), checks, ranks=ranks)
                    self.assertEqual(result['verdict'], 'fail')

    def test_failed_run_does_not_change_another_runs_verdict(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            checks = dict.fromkeys(ACCEPTANCE_CHECKS, True)
            for failure in ('temperature error', 'readback mismatch', 'invalid QoI'):
                with self.subTest(failure=failure):
                    failed = self.write_record(root, checks, [failure])
                    passed = self.write_record(root, checks)
                    self.assertEqual(failed['verdict'], 'fail')
                    self.assertEqual(passed['verdict'], 'pass')

    def test_main_raises_accumulated_failures_after_writing_results(self):
        try:
            import numpy  # noqa: F401
            import h5py  # noqa: F401
        except ImportError:
            self.skipTest('main requires numpy/h5py')
        output = StringIO()
        with tempfile.TemporaryDirectory() as directory:
            absent = str(Path(directory) / 'absent-executable')
            env = dict(CHIPCAE_IMPORT_EXE=absent, CHIPCAE_PREVIEW_EXE=absent,
                       CHIPCAE_SOLVE_EXE=absent)
            with patch.dict(os.environ, env), redirect_stdout(output), redirect_stderr(StringIO()):
                with self.assertRaises(AssertionError):
                    main()
        record = json.loads(output.getvalue().split('RESULT ranks1/result.json\n', 1)[1])
        self.assertEqual(record['verdict'], 'fail')
        self.assertEqual(record['qoi'], {})

    def test_retired_fixture_gate_does_not_skip_configured_backends(self):
        try:
            import numpy  # noqa: F401
            import h5py  # noqa: F401
        except ImportError:
            self.skipTest('main requires numpy/h5py')
        output = StringIO()
        with tempfile.TemporaryDirectory() as directory:
            absent = str(Path(directory) / 'absent-executable')
            env = dict(CHIPCAE_F1130_RUN_BLOCKED_ACCEPTANCE='0',
                       CHIPCAE_IMPORT_EXE=absent, CHIPCAE_PREVIEW_EXE=absent,
                       CHIPCAE_SOLVE_EXE=absent)
            with patch.dict(os.environ, env), redirect_stdout(output), redirect_stderr(StringIO()):
                with self.assertRaises(AssertionError):
                    main()
        self.assertNotIn('GATED:', output.getvalue())
        self.assertIn('RESULT ranks1/result.json', output.getvalue())

    def test_numerical_check_completes_only_after_all_inspection_assertions(self):
        import numpy as np
        import h5py
        import trace_map_ref as ref
        stack = synthetic_stack([.001])
        rings = {'In1.Cu': ref.PolyLayer(ref.rational(1),
            (((0,0),(ref.rational('.0005'),0),
              (ref.rational('.0005'),ref.rational('.001')),(0,ref.rational('.001'))),))}
        for failure in (None, 'temperature', 'readback', 'qoi'):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                (root / 'out').mkdir()
                warpage = -1e-6 if failure == 'qoi' else 1e-6
                write_json(root / 'manifest.json', dict(
                    schema_version='chipcae.results/1.0', summary=[
                        dict(name='warpage_pv',value=warpage),
                        dict(name='U_mag_max',value=2e-6), dict(name='T_max',value=336.62)]))
                write_json(root / 'summary.json', dict(warpage=dict(
                    manifest_warpage_pv_m=warpage*(1+6.8e-12) if failure == 'readback' else warpage,
                    w_bfp_m=2e-6)))
                write_synthetic_hex(root / 'out/step-0.vtkhdf',stack,patterned=True)
                if failure == 'temperature':
                    with h5py.File(root / 'out/step-0.vtkhdf','r+') as file:
                        # Unique external vertex: avoid triggering the boundary-copy check.
                        file['VTKHDF/hex/PointData/T'][0] += 1e-6
                measurements = dict(acceptance_checks={})
                # Geometry-specific area expectation; all numerical checks stay real.
                with patch.object(ref, 'COPPER_AREAS_MM2', {'In1.Cu': .5}), redirect_stdout(StringIO()):
                    if failure is None:
                        inspect_run(root,stack,rings,2,measurements,np,h5py)
                    else:
                        with self.assertRaises(AssertionError):
                            inspect_run(root,stack,rings,2,measurements,np,h5py)
                self.assertEqual(measurements['acceptance_checks'].get('numerical',False),
                                 failure is None)


class TraceMapReferenceTests(unittest.TestCase):
    """Independent oracle checks: geometry, Q2 physics and exported identities."""

    def test_exact_clipping_intersections_holes_and_empty_copper(self):
        from fractions import Fraction as F
        import trace_map_ref as ref
        square = ((1, 1), (3, 1), (3, 3), (1, 3))
        self.assertEqual(ref.column_fraction([square], (0, 0, 2, 2)), F(1, 4))
        self.assertEqual(ref.column_fraction([square], (0, 0, 1, 1)), F(0))
        self.assertEqual(ref.column_fraction([square], (0, 0, 4, 4)), F(1, 4))
        self.assertEqual(ref.column_fraction([square], (4, 4, 5, 5)), F(0))
        self.assertEqual(ref.column_fraction([], (0, 0, 4, 4)), F(0))
        outer = ((0, 0), (4, 0), (4, 4), (0, 4))
        hole = tuple(reversed(square))
        self.assertEqual(ref.column_fraction([outer, hole], (0, 0, 4, 4)), F(3, 4))
        triangle = ((0, 0), (2, 0), (0, 2))
        self.assertEqual(ref.column_fraction([triangle], (0, 0, 1, 1)), F(1))
        self.assertEqual(ref.column_fraction([triangle], (1, 0, 2, 1)), F(1, 2))
        for rings in ([square, square], [hole], [((0, 0), (2, 2), (0, 2), (2, 0))]):
            with self.subTest(rings=rings), self.assertRaisesRegex(AssertionError, 'ring|overlap|hole'):
                ref.column_fraction(rings, (0, 0, 4, 4))

    def test_polyset_binary_validation_and_exact_fixture_areas(self):
        from fractions import Fraction as F
        import trace_map_ref as ref
        path = SOURCE / 'tests/integration/import_cli/golden/board_4layer/stack.polyset'
        rings = ref.read_polyset(path)
        expected = {'F.Cu': F('0.307835'), 'In1.Cu': F('2.25'),
                    'In2.Cu': F(3), 'B.Cu': F(7)}
        for name, area in expected.items():
            layer = rings[name]
            actual = sum(ref.signed_area(r) for r in layer.rings) * layer.scale**2 * 10**6
            self.assertEqual(actual, area)
        with tempfile.TemporaryDirectory() as directory:
            bad = Path(directory) / 'bad.polyset'
            payload = path.read_bytes()
            for data in (b'', b'XXXXXXXX' + payload[8:], payload[:-1], payload + b'x'):
                bad.write_bytes(data)
                with self.subTest(size=len(data)), self.assertRaises(AssertionError):
                    ref.read_polyset(bad)

    def test_q2_basis_partition_derivatives_and_symmetric_assembly(self):
        import numpy as np
        import trace_map_ref as ref
        for x in (0, .13, .5, .81, 1):
            values, derivatives = ref.basis(x)
            self.assertLessEqual(abs(sum(values) - 1), 1e-14)
            self.assertLessEqual(abs(sum(derivatives)), 1e-14)
        stack = synthetic_stack([.001])
        mesh = ref.tensor_mesh(stack, 1, 1)
        operator, rhs, dofs = ref.assemble_q2(mesh, np.array([[195.15, 195.15, 780/1301]]))
        matrix = operator.toarray() if hasattr(operator, 'toarray') else operator
        self.assertLessEqual(np.max(abs(matrix - matrix.T)) / np.max(abs(matrix)), 1e-12)
        self.assertEqual(len(dofs['points']), 27)
        self.assertTrue(np.all(np.isfinite(rhs)))

    def test_single_column_known_fraction_and_numpy_fallback(self):
        import numpy as np
        import trace_map_ref as ref
        # Analytic 50% copper/FR4 series solution, independent literal values.
        stack = synthetic_stack([.001])
        mesh = ref.tensor_mesh(stack, 1, 1)
        k = np.array([[195.15, 195.15, 780/1301]])
        operator, rhs, dofs = ref.assemble_q2(mesh, k)
        sparse, _ = ref.solve_q2(operator, rhs, dofs)
        # Force assembly as well as the direct solve to operate without SciPy.
        with patch.dict(sys.modules, {'scipy': None, 'scipy.sparse': None}):
            numpy_operator, numpy_rhs, numpy_dofs = ref.assemble_q2(mesh, k)
            dense, reactions = ref.solve_q2(numpy_operator, numpy_rhs, numpy_dofs, use_scipy=False)
        self.assertIsInstance(numpy_operator, np.ndarray)
        for point, actual in zip(dofs['points'], dense, strict=True):
            want = 298.15 + (float(point[2]) / .001) * (65050/2081)
            self.assertLessEqual(abs(actual - want), 1e-8)
        self.assertLessEqual(np.max(abs(sparse - dense)), 1e-8)
        qtop, qbot, rlo, rhi = ref.thermal_checks(mesh, stack, k, dense, reactions, dofs)
        self.assertLessEqual(abs(qtop - qbot) / abs(qbot), 1e-9)
        self.assertLessEqual(abs(qbot - .03748197981739548), 1e-11)
        self.assertLessEqual(abs(rlo - rhi), 1e-9)
        corner = ref.corner_values(mesh, dense, dofs)
        recovered, recovered_reactions = ref.solve_q2(operator, rhs, dofs, corners=corner)
        self.assertLessEqual(np.max(abs(recovered - dense)), 1e-8)
        self.assertLessEqual(np.max(abs(recovered_reactions - reactions)), 1e-10)

    def test_uniform_multilayer_series_solution(self):
        import numpy as np
        import trace_map_ref as ref
        stack = synthetic_stack([.0002, .001, .0003])
        mesh = ref.tensor_mesh(stack, 2, 1)
        # Uniform f=(0, 1/2, 1); independent series resistance in SI.
        kz = [.3, 780/1301, 390]
        k = np.array([[.3, .3, .3], [195.15, 195.15, kz[1]], [390, 390, 390]] * 2)
        operator, rhs, dofs = ref.assemble_q2(mesh, k)
        temperatures, reactions = ref.solve_q2(operator, rhs, dofs)
        flux = 50 / (.0002/.3 + .001/(780/1301) + .0003/390 + .001)
        for point, actual in zip(dofs['points'], temperatures, strict=True):
            z = float(point[2])
            want = 298.15 + flux * (min(z, .0002)/.3 +
                min(max(z-.0002, 0), .001)/(780/1301) + max(z-.0012, 0)/390)
            self.assertLessEqual(abs(actual - want), 1e-8)
        qtop, qbot, _, _ = ref.thermal_checks(mesh, stack, k, temperatures, reactions, dofs)
        self.assertLessEqual(abs(qtop-qbot)/abs(qbot), 1e-9)

    def test_partition_permutations_align_cells_and_vertices(self):
        import trace_map_ref as ref
        stack = synthetic_stack([.001])
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            meshes = []
            for order in ((0, 1), (1, 0)):
                path = root / f'{order[0]}.vtkhdf'
                write_synthetic_hex(path, stack, order=order)
                import h5py
                with h5py.File(path) as file:
                    meshes.append(ref.read_hex_mesh(file['VTKHDF/hex'], stack))
            self.assertEqual(meshes[0]['bounds'], meshes[1]['bounds'])
            self.assertEqual(meshes[0]['vertices'], meshes[1]['vertices'])
            ref.compare_aligned(meshes[0], meshes[1], PARALLEL_RTOL)
            for field in ('copper_fraction', 'conductivity_diag', 'temperatures', 'displacement'):
                mutated = deepcopy(meshes[1])
                values = mutated['fields'][field] if field in mutated['fields'] else mutated[field]
                values.flat[0] += .001
                with self.subTest(field=field), self.assertRaisesRegex(AssertionError, 'parallel'):
                    ref.compare_aligned(meshes[0], mutated, PARALLEL_RTOL)

    def test_invalid_topology_identity_and_missing_fields_fail(self):
        import h5py
        import trace_map_ref as ref
        stack = synthetic_stack([.001])
        for defect in ('duplicate_gid', 'missing_field', 'layer_id', 'connectivity',
                       'boundary_temperature', 'missing_cell', 'wrong_gid', 'missing_fraction'):
            with self.subTest(defect=defect), tempfile.TemporaryDirectory() as directory:
                path = Path(directory) / 'mesh.vtkhdf'
                write_synthetic_hex(path, stack)
                with h5py.File(path, 'r+') as file:
                    group = file['VTKHDF/hex']
                    if defect == 'duplicate_gid':
                        group['CellData/elem_gid'][1] = 0
                    elif defect == 'wrong_gid':
                        group['CellData/elem_gid'][:] = [1, 0]
                    elif defect == 'missing_field':
                        del group['CellData/conductivity_diag']
                    elif defect == 'missing_fraction':
                        del group['CellData/copper_fraction']
                    elif defect == 'layer_id':
                        group['CellData/layer_id'][0] = 2
                    elif defect == 'connectivity':
                        group['Connectivity'][1] = 0
                    elif defect == 'boundary_temperature':
                        group['PointData/T'][8] += 1e-6
                    else:
                        group['NumberOfCells'][1] = 0
                    with self.assertRaises(AssertionError):
                        ref.read_hex_mesh(group, stack)

    def test_distinct_hex_corners_with_crossed_connectivity_are_rejected(self):
        import h5py
        import trace_map_ref as ref
        stack = synthetic_stack([.001])
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'mesh.vtkhdf'
            write_synthetic_hex(path, stack)
            with h5py.File(path, 'r+') as file:
                group = file['VTKHDF/hex']
                # All eight corners remain present, but the bottom face crosses.
                group['Connectivity'][:2] = [1, 0]
                with self.assertRaisesRegex(AssertionError, 'hex connectivity'):
                    ref.read_hex_mesh(group, stack)

    def test_tensor_reference_rejects_nonrectangular_board_outlines(self):
        import trace_map_ref as ref
        stack = synthetic_stack([.001])
        # The box alone must not silently replace a different physical domain.
        for polygon in ([[0,0],[.002,0],[0,.001]],
                        [[0,0],[.002,.001],[.002,0],[0,.001]]):
            stack['outline']['polygon'] = polygon
            with self.subTest(polygon=polygon), self.assertRaisesRegex(
                    AssertionError, 'unsupported board outline'):
                ref.tensor_mesh(stack,2,1)

    def test_cell_fraction_conductivity_and_temperature_mutations_fail(self):
        import h5py
        import trace_map_ref as ref
        stack = synthetic_stack([.001])
        # Half of the first xy column is copper, second is empty.
        rings = {'In1.Cu': ref.PolyLayer(ref.rational(1),
            (((0, 0), (ref.rational('.0005'), 0),
              (ref.rational('.0005'), ref.rational('.001')), (0, ref.rational('.001'))),))}
        for defect in (None, 'zero_copper', 'nan', 'inf', 'negative', 'above_one',
                       'conductivity', 'temperature', 'case', 'missing_layer', 'conductivity_nan',
                       'conductivity_inf', 'area'):
            with self.subTest(defect=defect), tempfile.TemporaryDirectory() as directory:
                path = Path(directory) / 'mesh.vtkhdf'
                write_synthetic_hex(path, stack, patterned=True)
                with h5py.File(path) as file:
                    mesh = ref.read_hex_mesh(file['VTKHDF/hex'], stack)
                given_rings = dict(rings)
                if defect in ('zero_copper', 'nan', 'inf', 'negative', 'above_one'):
                    mesh['fields']['copper_fraction'][0] = {
                        'zero_copper': 0, 'nan': float('nan'), 'inf': float('inf'),
                        'negative': -.01, 'above_one': 1.01}[defect]
                elif defect == 'area':
                    # Below the cell tolerance, but above relative 1e-12 for area.
                    mesh['fields']['copper_fraction'][0] += 8e-13
                elif defect in ('conductivity_nan', 'conductivity_inf'):
                    mesh['fields']['conductivity_diag'][0,0] = float(
                        'nan' if defect == 'conductivity_nan' else 'inf')
                elif defect == 'conductivity':
                    mesh['fields']['conductivity_diag'][0, 2] *= 1.000001
                elif defect == 'case':
                    given_rings['in1.cu'] = given_rings.pop('In1.Cu')
                elif defect == 'missing_layer':
                    given_rings.clear()
                elif defect == 'temperature':
                    mesh['temperatures'][-1] += 1e-6
                if defect is None:
                    ref.check_solution(mesh, given_rings, stack, {}, expected_areas={'In1.Cu': .5})
                else:
                    with self.assertRaises(AssertionError):
                        ref.check_solution(mesh, given_rings, stack, {}, expected_areas={'In1.Cu': .5})

    def test_multiz_copper_areas_and_wiener_bounds_use_volume_weights(self):
        import numpy as np
        import trace_map_ref as ref
        stack = synthetic_stack([.001])
        mesh = ref.tensor_mesh(stack,2,1,[ref.rational(0),ref.rational('.0004'),ref.rational('.001')])
        rings = {'In1.Cu': ref.PolyLayer(ref.rational(1),
            (((0,0),(ref.rational('.0005'),0),
              (ref.rational('.0005'),ref.rational('.001')),(0,ref.rational('.001'))),))}
        phi,k = ref.reference_coefficients(mesh,rings,stack)
        np.testing.assert_allclose(phi,[.5,.5,0,0],rtol=0,atol=1e-15)
        op,rhs,dofs = ref.assemble_q2(mesh,k)
        temperatures,reactions = ref.solve_q2(op,rhs,dofs)
        mesh['temperatures'] = np.array(list(ref.corner_values(mesh,temperatures,dofs).values()))
        mesh['fields'] = dict(copper_fraction=phi,conductivity_diag=k)
        measurements = {}
        ref.check_solution(mesh,rings,stack,measurements,expected_areas={'In1.Cu': .5})
        self.assertLessEqual(abs(measurements['copper_areas_mm2']['In1.Cu']-.5), 1e-12)
        qtop,qbot,rlo,rhi = ref.thermal_checks(mesh,stack,k,temperatures,reactions,dofs)
        # A=2e-6; arithmetic/harmonic kz means are hand-derived.
        expected_lo = .001/(((780/1301+.3)/2)*2e-6)+500
        expected_hi = .001*((1301/780+1/.3)/2)/2e-6+500
        self.assertLessEqual(abs(rlo-expected_lo)/expected_lo,1e-14)
        self.assertLessEqual(abs(rhi-expected_hi)/expected_hi,1e-14)
        epsilon = 1e-9*max(50/rlo,50/rhi,abs(qbot),1e-30)
        self.assertLessEqual(50/rhi-epsilon,qbot)
        self.assertLessEqual(qbot,50/rlo+epsilon)
        self.assertLessEqual(abs(qtop-qbot)/abs(qbot),1e-9)

    def test_missing_layer_extent_and_cross_layer_cells_are_rejected(self):
        import trace_map_ref as ref
        stack = synthetic_stack([.0002,.001,.0003])
        for z in ([0,.0002,.0012], [0,.0002,.0015], [0,.0015],
                  [0,.0002,.0012,.0012,.0015]):
            with self.subTest(z=z), self.assertRaises(AssertionError):
                ref.tensor_mesh(stack,1,1,list(map(ref.rational,z)))

    def test_core_coefficients_are_fixed_fr4_and_zero_copper(self):
        import numpy as np
        import trace_map_ref as ref
        stack = synthetic_stack([.001])
        stack['layers'][0] = dict(name='Core',kind='Homogenized',material='fr4_eq',thickness=.001)
        mesh = ref.tensor_mesh(stack,1,1)
        phi,k = ref.reference_coefficients(mesh,{},stack)
        np.testing.assert_allclose(phi,[0],rtol=0,atol=1e-15)
        np.testing.assert_allclose(k,[[.3,.3,.3]],rtol=1e-14,atol=0)
        stack['layers'][0]['material'] = 'cu'
        with self.assertRaisesRegex(AssertionError,'non-copper material'):
            ref.reference_coefficients(mesh,{},stack)


def synthetic_stack(thicknesses):
    return dict(outline=dict(box=[0, 0, .002, .001]), layers=[
        dict(name='In1.Cu' if len(thicknesses) == 1 else f'L{i}',
             material='fr4_eq', kind='Patterned', thickness=t,
             copper=dict(metal='cu', polygons=dict(file='synthetic.polyset',
                 layer='In1.Cu' if len(thicknesses) == 1 else f'L{i}')))
        for i, t in enumerate(thicknesses)])


def write_synthetic_hex(path, stack, order=(0, 1), patterned=False):
    """Two true hex partitions, each with local connectivity restarting at zero."""
    import h5py
    import numpy as np
    import trace_map_ref as ref
    mesh = ref.tensor_mesh(stack, 2, 1)
    k = np.array([[195.15, 195.15, 780/1301] if patterned and i == 0 else [.3]*3
                  for i in range(2)])
    operator, rhs, dofs = ref.assemble_q2(mesh, k)
    temperatures, _ = ref.solve_q2(operator, rhs, dofs)
    corners = ref.corner_values(mesh, temperatures, dofs)
    points, values = [], []
    for i in order:
        x0, y0, z0, x1, y1, z1 = mesh['bounds'][i]
        vertices = [(x0,y0,z0), (x1,y0,z0), (x1,y1,z0), (x0,y1,z0),
                    (x0,y0,z1), (x1,y0,z1), (x1,y1,z1), (x0,y1,z1)]
        points.extend(vertices)
        values.extend(corners[p] for p in vertices)
    with h5py.File(path, 'w') as file:
        file['VTKHDF/cut/NumberOfCells'] = [0, 0]
        group = file.create_group('VTKHDF/hex')
        for name, data in dict(NumberOfPoints=[8,8], NumberOfCells=[1,1],
                NumberOfConnectivityIds=[8,8], Connectivity=list(range(8))*2,
                Offsets=[0,8,0,8], Types=[12,12], Points=np.array(points, dtype=float),
                **{'PointData/T': values, 'PointData/U': np.zeros((16,3)),
                   'CellData/elem_gid': list(order), 'CellData/layer_id': [1,1],
                   'CellData/cell_kind': [0,0],
                   'CellData/copper_fraction': [.5 if patterned and i == 0 else 0. for i in order],
                   'CellData/conductivity_diag': k[list(order)]}).items():
            group[name] = data


if __name__ == '__main__':
    suite = unittest.defaultTestLoader.loadTestsFromModule(sys.modules[__name__])
    if not unittest.TextTestRunner().run(suite).wasSuccessful():
        raise SystemExit(1)
    raise SystemExit(main())
