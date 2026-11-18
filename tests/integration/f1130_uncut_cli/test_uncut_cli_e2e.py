"""Public synthetic four-layer IPC → uncut CLI chain, serial/MPI, analytic T."""

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

SOURCE = Path(sys.argv[1]).resolve()
sys.path.insert(0, str(SOURCE))

TEMPERATURE_ATOL = 1e-8
READBACK_RTOL = 1e-12
PARALLEL_RTOL = 1e-8
QOIS = ('warpage_pv', 'U_mag_max', 'T_max')
ACCEPTANCE_CHECKS = ('import', 'fractions', 'preview', 'solve_readback',
                     'inputs_unchanged', 'numerical')


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
    assert len(actual) == len(expected), (
        f'M1 layer count: actual={len(actual)} expected={len(expected)}')
    for index, (got, want) in enumerate(zip(actual, expected, strict=True)):
        context = f'M1 layer[{index}] {got.get("name")!r}'
        for key in ('name', 'material'):
            assert got[key] == want[key], (
                f'{context} {key}: actual={got[key]!r} expected={want[key]!r}')
        assert math.isclose(got['thickness'], want['thickness'], rel_tol=1e-12, abs_tol=0), (
            f'{context} thickness: actual={got["thickness"]!r} '
            f'expected={want["thickness"]!r}')


def compare_fractions(report, converted, imported, measurements):
    copper_names = ['F.Cu', 'In1.Cu', 'In2.Cu', 'B.Cu']
    entries = report['layers']
    names = [entry['layer'] for entry in entries]
    assert len(names) == 4 and sorted(names) == sorted(copper_names), (
        f'M1 copper report: actual={names!r} expected exactly once={copper_names!r}')
    fractions = {entry['layer']: entry['fraction'] for entry in entries}
    measurements['copper_fractions'] = {
        name: value if math.isfinite(value) else None for name, value in fractions.items()
    }
    print(f'实测 copper fractions: {fractions}', flush=True)
    assert len(converted['layers']) == len(imported['layers'])
    for index, (layer, original) in enumerate(zip(converted['layers'], imported['layers'], strict=True)):
        assert layer['name'] == original['name'], f'converted layer[{index}] order changed'
        assert layer['material'] == original['material'], f'converted layer[{index}] material changed'
        if layer['name'] in fractions:
            copper = layer['copper']
            assert copper['metal'] == 'mat.cu', layer
            assert 'polygons' not in copper, layer
            assert math.isclose(copper['fraction'], fractions[layer['name']],
                                rel_tol=1e-12, abs_tol=0), layer
    # Saved golden F.Cu polygon area: 0.307835 mm² / 25 mm² board area.
    # The mandated fixture has no geometry on the other copper layers.
    for name, expected in zip(copper_names, (.0123134, 0., 0., 0.), strict=True):
        fraction = fractions[name]
        assert math.isfinite(fraction) and 0 < fraction <= 1, (
            f'M1 copper fraction {name}: actual={fraction!r} expected 0 < fraction <= 1; '
            f'all fractions={fractions!r}')
        assert math.isclose(fraction, expected, rel_tol=1e-12, abs_tol=0), (
            f'M1 copper fraction {name}: actual={fraction!r} expected={expected}')


def write_solver_inputs(root):
    # Illustrative definitions and case fields copied from test_ipc_uncut_chain.py.
    materials = []
    for name, E, nu, alpha, k in [('mat.cu', 110e9, .34, 17e-6, 390),
                                 ('mat.fr4', 25e9, .3, 1e-5, .3)]:
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
        schema='chipcae.case/1.0', stack=dict(ref='solve.stack.json'),
        materials=dict(ref='materials.json'),
        mesh=dict(type='aligned_cartesian_cut', nx=4, ny=4, levels=0, max_ratio=100,
                  min_elems_per_layer=1),
        physics=[dict(id='thermal', solver='fem.conduction', mode='steady'),
                 dict(id='mech', solver='fem.solid', temp_from='thermal', T_ref=298.15)],
        loads=[dict(type='fixed_temperature', target='face:zmin', T=298.15),
               dict(type='convection', target='face:zmax', h=1000, T_inf=348.15)],
        constraints=dict(rigid='3-2-1'), solver=dict(linear=dict(preconditioner='direct')),
        output=dict(format='vtkhdf')))
    for ranks in (1, 2):
        write_json(root / f'profile_r{ranks}.json', dict(
            schema='chipcae.run_profile/1.0', backend='cpu', ranks=ranks, gpus=0))
    p1, p2 = (read_json(root / f'profile_r{ranks}.json') for ranks in (1, 2))
    assert p1.pop('ranks') == 1 and p2.pop('ranks') == 2 and p1 == p2


def inspect_run(run_dir, stack, preview_count, measurements, np, h5py):
    """Inspect exported points; analytic values never come from the solver."""
    manifest = read_json(run_dir / 'manifest.json')
    assert manifest['schema_version'] == 'chipcae.results/1.0', manifest.get('schema_version')
    summary = {entry['name']: entry['value'] for entry in manifest['summary']}
    for key in QOIS:
        assert math.isfinite(summary[key]), (key, summary)
        if key != 'T_max':
            assert summary[key] > 0, (key, summary)
    measurements['qoi'] = {key: summary[key] for key in QOIS}
    conductivity = {'mat.cu': 390, 'mat.fr4': .3}
    layers = []
    for layer in stack['layers']:
        k = conductivity[layer['material']]
        if 'copper' in layer:
            copper = layer['copper']
            f = copper['fraction']
            k = 1 / (f / conductivity[copper['metal']] + (1 - f) / k)
        layers.append((layer['thickness'], k))
    heat_flux = 50 / (sum(t / k for t, k in layers) + 1 / 1000)
    with h5py.File(artifact(run_dir / 'out/step-0.vtkhdf'), 'r') as file:
        cut_count = int(np.sum(file['VTKHDF/cut/NumberOfCells'][:]))
        hexes = file['VTKHDF/hex']
        count = int(np.sum(hexes['NumberOfCells'][:]))
        measurements.update(cut_cells=cut_count, hex_cells=count, preview_cells=preview_count)
        assert cut_count == 0, f'M3 cut cells={cut_count}'
        assert count > 0 and count == preview_count, (
            f'M2/M3 hex cells={count} preview cell_count={preview_count}')
        assert np.all(hexes['CellData/cell_kind'][:] == 0), 'M3 nonzero hex cell_kind'
        z = hexes['Points'][:, 2]
        assert math.isclose(float(np.min(z)), 0.0, abs_tol=1e-15), 'M3 zmin'
        assert math.isclose(float(np.max(z)), sum(t for t, _ in layers), rel_tol=1e-12), 'M3 zmax'
        resistance = np.zeros_like(z)
        bottom = 0.0
        for thickness, k in layers:
            resistance += np.clip(z - bottom, 0, thickness) / k
            bottom += thickness
        expected = 298.15 + heat_flux * resistance
        temperatures = hexes['PointData/T'][:]
        assert temperatures.shape == expected.shape, (temperatures.shape, expected.shape)
        assert np.all(np.isfinite(temperatures)), 'M3 nonfinite temperatures'
        error = float(np.max(np.abs(temperatures - expected)))
        assert math.isfinite(error), 'M3 nonfinite temperature error'
        measurements['max_temperature_error_K'] = error
    readback = read_json(run_dir / 'summary.json')['warpage']['w_bfp_m']
    assert readback is not None and math.isfinite(readback), f'M5 readback warpage={readback!r}'
    relative = abs(readback - summary['warpage_pv']) / abs(summary['warpage_pv'])
    measurements.update(readback_warpage_m=readback, readback_relative_error=relative)
    print(f'实测 {run_dir.name}: {json.dumps(measurements, ensure_ascii=False)}', flush=True)
    # Both solves/readbacks have completed before either numerical assertion.
    assert error <= TEMPERATURE_ATOL, (
        f'M3 {run_dir.name} max temperature error={error:.17g} K > {TEMPERATURE_ATOL} K')
    assert relative <= READBACK_RTOL, (
        f'M3/M5 {run_dir.name} warpage readback relative error={relative:.17g} > {READBACK_RTOL}')
    measurements['acceptance_checks']['numerical'] = True


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
    required = list(ACCEPTANCE_CHECKS)
    if ranks == 2 or measurements.get('parallel_required', False):
        required.append('parallel')
    checks = measurements.get('acceptance_checks', {})
    failures += [f'acceptance check incomplete: {name}' for name in required
                 if checks.get(name) is not True]
    commit = get_commit(notes)
    notes += [f'实测 {stage}: {seconds:.6f} s' for stage, seconds in timings.items()]
    notes += failures
    result = dict(
        schema='chipcae.vv_result/0.2', case_id='f1130_uncut_board4',
        data_class='synthetic', label='实测', date=datetime.now().strftime('%Y%m%d'),
        commit=commit, verdict='fail' if failures else 'pass',
        reference=dict(value=None, type='解析', source='1D 串联热阻，test_ipc_uncut_chain.py 同口径'),
        qoi=measurements.get('qoi', {}), run=dict(ranks=ranks, **measurements),
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
        # Keep an auditable failure record even if import/conversion blocks solving.
        run1 = root / 'ranks1'
        run1.mkdir()
        runs = {1: dict(path=run1, measurements=dict(acceptance_checks={}),
                       completed=False, failures=[])}
        setup_checks = runs[1]['measurements']['acceptance_checks']
        try:
            material_map = SOURCE / 'tests/integration/import_cli/material_map.json'
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
            run_command('fraction conversion', [sys.executable, SOURCE / 'tools/stack/copper_fraction.py',
                                               'stack.json', '-o', 'solve.stack.json',
                                               '--report', 'cf.json'], root, timings)
            stack = read_json(root / 'solve.stack.json')
            compare_fractions(read_json(root / 'cf.json'), stack, imported, runs[1]['measurements'])
            setup_checks['fractions'] = True
            stdout = run_command('preview', [previewer, 'solve.stack.json', '-o', 'preview',
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
                      ('solve.stack.json', 'case.json', 'materials.json',
                       'profile_r1.json', 'profile_r2.json')}
            launcher = shutil.which('mpiexec') or shutil.which('mpirun')
            if launcher:
                run2 = root / 'ranks2'
                run2.mkdir()
                runs[2] = dict(path=run2, measurements=dict(
                    acceptance_checks=dict(setup_checks)), completed=False, failures=[])
                for run in runs.values():
                    run['measurements']['parallel_required'] = True
            else:
                print('SKIP ranks=2: launcher not found', flush=True)
                notes.append('SKIP ranks=2: launcher not found; serial acceptance retained.')
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
                        inspect_run(run['path'], stack, preview_count, run['measurements'], np, h5py)
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


CHECKS = ('import', 'fractions', 'preview', 'solve_readback', 'inputs_unchanged', 'numerical')


class AcceptanceRecords(unittest.TestCase):
    def test_import_rejects_changed_literal_material_id(self):
        golden = read_json(SOURCE / 'tests/integration/import_cli/golden/board_4layer/stack.json')
        original = json.dumps(golden)
        imported = json.loads(original)
        compare_import(imported, golden)
        imported['layers'][0]['material'] = 'mat_cu'
        with self.assertRaisesRegex(AssertionError, 'material:'):
            compare_import(imported, golden)
        self.assertEqual(json.dumps(golden), original)

    def write_record(self, root, checks, failures=(), ranks=1, parallel_required=False):
        measurements = dict(
            acceptance_checks=checks, parallel_required=parallel_required,
            qoi=dict(warpage_pv=1e-6, U_mag_max=2e-6, T_max=318.15),
            max_temperature_error_K=1e-10)
        with redirect_stdout(StringIO()):
            write_result(root, ranks, measurements, {}, [], list(failures))
        return json.loads((root / 'result.json').read_text(encoding='utf-8'))

    def test_each_missing_acceptance_check_prevents_pass(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for missing in CHECKS:
                with self.subTest(missing=missing):
                    checks = {name: True for name in CHECKS if name != missing}
                    result = self.write_record(root, checks)
                    self.assertEqual(result['verdict'], 'fail')
                    self.assertTrue(any(missing in note for note in result['notes']))

    def test_empty_numerical_record_cannot_pass(self):
        with tempfile.TemporaryDirectory() as directory, redirect_stdout(StringIO()):
            root = Path(directory)
            write_result(root, 1, {}, {}, [], [])
            result = json.loads((root / 'result.json').read_text(encoding='utf-8'))
            self.assertEqual(result['verdict'], 'fail')

    def test_parallel_comparison_is_required_for_both_available_runs(self):
        with tempfile.TemporaryDirectory() as directory:
            for ranks in (1, 2):
                with self.subTest(ranks=ranks):
                    result = self.write_record(Path(directory), dict.fromkeys(CHECKS, True),
                                               ranks=ranks, parallel_required=True)
                    self.assertEqual(result['verdict'], 'fail')

    def test_failed_run_does_not_change_another_runs_verdict(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            checks = dict.fromkeys(CHECKS, True)
            for failure in ('temperature error', 'readback mismatch', 'invalid QoI'):
                with self.subTest(failure=failure):
                    failed = self.write_record(root, checks, [failure])
                    passed = self.write_record(root, checks)
                    self.assertEqual(failed['verdict'], 'fail')
                    self.assertEqual(passed['verdict'], 'pass')

    def test_main_raises_accumulated_failures_after_writing_gated_results(self):
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

    def test_numerical_check_completes_only_after_all_inspection_assertions(self):
        try:
            import numpy as np
            import h5py
        except ImportError:
            self.skipTest('inspection requires numpy/h5py')
        # One 1 mm FR4 layer: R=1/300+1/1000, q=150000/13 W/m².
        # T(top)=298.15+500/13 K, independently evaluated here as a literal.
        for failure in (None, 'temperature', 'readback', 'qoi'):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                (root / 'out').mkdir()
                warpage = -1e-6 if failure == 'qoi' else 1e-6
                write_json(root / 'manifest.json', dict(
                    schema_version='chipcae.results/1.0', summary=[
                        dict(name='warpage_pv', value=warpage),
                        dict(name='U_mag_max', value=2e-6), dict(name='T_max', value=336.62)]))
                write_json(root / 'summary.json', dict(
                    warpage=dict(w_bfp_m=1.01e-6 if failure == 'readback' else 1e-6)))
                with h5py.File(root / 'out/step-0.vtkhdf', 'w') as file:
                    file['VTKHDF/cut/NumberOfCells'] = [0]
                    file['VTKHDF/hex/NumberOfCells'] = [1]
                    file['VTKHDF/hex/CellData/cell_kind'] = [0]
                    file['VTKHDF/hex/Points'] = [[0., 0., 0.], [.001, .001, .001]]
                    top = 336.61153846153843 + (.001 if failure == 'temperature' else 0)
                    file['VTKHDF/hex/PointData/T'] = [298.15, top]
                measurements = dict(acceptance_checks={})
                stack = dict(layers=[dict(material='mat.fr4', thickness=.001)])
                with redirect_stdout(StringIO()):
                    if failure is None:
                        inspect_run(root, stack, 1, measurements, np, h5py)
                    else:
                        with self.assertRaises(AssertionError):
                            inspect_run(root, stack, 1, measurements, np, h5py)
                self.assertEqual(measurements['acceptance_checks'].get('numerical', False),
                                 failure is None)


if __name__ == '__main__':
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(AcceptanceRecords)
    if not unittest.TextTestRunner().run(suite).wasSuccessful():
        raise SystemExit(1)
    raise SystemExit(main())
