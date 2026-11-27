"""CLI guards for FVM preconditioner selection and failed-solve publication."""

import argparse
from copy import deepcopy
import json
import math
import os
from pathlib import Path
import shutil
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'integration' / 'fvm_case_e2e'))
from fvm_case_util import (decode_events, elastic, energy, fixture, read_json, solve, thermal_case,
                          write_json)


def configure(exe, mpiexec):
    os.environ['CHIPCAE_SOLVE_EXE'] = str(Path(exe).resolve())
    os.environ['CHIPCAE_MPIEXEC'] = mpiexec
    os.environ.setdefault('CHIPCAE_MPIEXEC_NUMPROC_FLAG', '-n')


def output_text(out, process):
    text = process.stdout + '\n' + process.stderr
    path = out / 'events.bin'
    if path.exists():
        text += '\n' + '\n'.join(event.get('text', '')
                                  for event in decode_events(path.read_bytes()))
    return text


def check(checks, name, condition, detail):
    print(f'{name}: {"PASS" if condition else "FAIL"}: {detail}', flush=True)
    checks.append((name, bool(condition)))


def no_success(out, checks, name):
    manifest = out / 'manifest.json'
    steps = read_json(manifest)['steps'] if manifest.exists() else []
    check(checks, name + ' no successful manifest step', steps == [], steps)
    files = list(out.rglob('*.vtkhdf'))
    check(checks, name + ' no published step file', not files, files)


def failed_solve(out, process, checks, name):
    text = output_text(out, process)
    print(f'{name} actual output:\n{text}', flush=True)
    check(checks, name + ' nonzero exit', process.returncode != 0, process.returncode)
    check(checks, name + ' nonconvergence', 'nonconvergence' in text, text)
    no_success(out, checks, name)


def selected_pc(out, process, checks, name, expected):
    text = output_text(out, process)
    check(checks, name + ' successful exit', process.returncode == 0,
          f'exit={process.returncode}\n{text}')
    # Separate JSON diagnostics preserve the strict energy/elastic log contracts.
    events = decode_events((out / 'events.bin').read_bytes())
    energy(events)
    elastic(events)
    lines = [event for event in events if event['kind'] == 20 and
             event['text'].startswith('FVM_LINEAR ')]
    records = [json.loads(event['text'][len('FVM_LINEAR '):]) for event in lines]
    check(checks, name + ' linear diagnostics',
          len(records) == 2 and
          all(event['level'] == 1 and event['rank'] == 0 for event in lines) and
          all(isinstance(record, dict) and set(record) == {'field', 'pc', 'its', 'residual'}
              for record in records) and
          [record['field'] for record in records] == ['thermal', 'elastic'] and
          all(type(record['its']) is int and record['its'] >= 0 and
              type(record['residual']) in (int, float) and
              math.isfinite(record['residual']) and record['residual'] >= 0
              for record in records), records)
    check(checks, name + ' preconditioner', bool(records) and
          all(record.get('pc') == expected for record in records),
          f'expected pc={expected}; diagnostics={records!r}\n{text}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('exe')
    parser.add_argument('mpiexec')
    parser.add_argument('--failure-only', action='store_true')
    args = parser.parse_args()
    configure(args.exe, args.mpiexec)
    checks = []
    with tempfile.TemporaryDirectory(prefix='fvm-convergence-') as tmp:
        root = Path(tmp)
        fixture(root)
        library = read_json(root / 'materials.json')
        # Successful thermal runs continue into mechanics: isolate that stage.
        for card in library['materials']:
            card['thermal_expansion']['table']['columns']['alpha'] = [0]
        write_json(root / 'materials.json', library)
        case = thermal_case()
        # Two materials, 4x4 CVs per layer, asymmetric loading: Jacobi cannot
        # solve this in a single iteration (the failure-only run verifies it).
        case['loads'].append(dict(type='fixed_temperature', target='face:xmin', T=325))
        forced = deepcopy(case)
        forced['solver']['linear'].update(preconditioner='jacobi', max_iter=1, rtol=1e-14)
        out, process = solve(root, forced, 'thermal-failure-np1', expected=None)
        failed_solve(out, process, checks, 'thermal-failure-np1')
        if shutil.which(args.mpiexec):
            out, process = solve(root, forced, 'thermal-failure-np2', ranks=2, expected=None)
            failed_solve(out, process, checks, 'thermal-failure-np2')
        else:
            print('thermal-failure-np2: 未执行: MPI launcher unavailable', flush=True)
        if not args.failure_only:
            default = deepcopy(case)
            del default['solver']['linear']['preconditioner']
            out, process = solve(root, default, 'default', expected=None)
            selected_pc(out, process, checks, 'default', 'gamg')
            out, process = solve(root, case, 'explicit-jacobi', expected=None)
            selected_pc(out, process, checks, 'explicit-jacobi', 'jacobi')
    failures = [name for name, passed in checks if not passed]
    assert not failures, 'Failed acceptance assertions: ' + ', '.join(failures)


if __name__ == '__main__':
    main()
