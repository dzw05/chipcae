"""Measure registered solve CTests, retaining each independent wall time."""

import argparse
import json
import math
from pathlib import Path
import subprocess
import sys
import tempfile
import time
from xml.etree import ElementTree as ET


def parse_arguments(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', required=True)
    parser.add_argument('--test', required=True, choices=('solve_mesh', 'solve_pipeline'))
    parser.add_argument('--repeat', type=int, choices=range(1, 4), default=1)
    parser.add_argument('--out', required=True)
    args = parser.parse_args(argv)
    if not args.out.strip():
        parser.error('output path must not be empty')
    return args


def exception_message(error):
    return f'{type(error).__name__}: {error}'


def check_registration(build, test, runner):
    """Return an empty error only for exactly one registered target."""
    try:
        result = runner(['ctest', '--test-dir', build, '--show-only=json-v1'],
                        capture_output=True, text=True)
        if result.returncode != 0:
            return f'CTest discovery exited with {result.returncode}'
        data = json.loads(result.stdout)
        if data.get('kind') != 'ctestInfo' or not isinstance(data.get('tests'), list):
            return 'invalid CTest registration report'
        names = [entry['name'] for entry in data['tests']]
        count = names.count(test)
        if count == 0:
            return 'test not registered'
        if count != 1:
            return 'test registration is not unique'
    except (OSError, ValueError, TypeError, KeyError, AttributeError) as error:
        return exception_message(error)
    return ''


def report_status(report, test):
    """Read execution status without treating an empty report as success."""
    root = ET.parse(report).getroot()
    cases = list(root.iter('testcase'))
    if len(cases) != 1 or cases[0].get('name') != test:
        return 'failed', 'expected exactly one matching test result'
    case = cases[0]
    if case.find('failure') is not None or case.find('error') is not None:
        return 'failed', 'CTest reported test failure'
    if case.find('skipped') is not None or case.get('status') in ('notrun', 'disabled'):
        return 'skipped', 'CTest reported test skipped'
    return 'passed', ''


def run_once(build, test, index, report, runner, clock):
    run = dict(index=index, returncode=None, status='failed', wall_seconds=0.0, error='')
    started = clock()
    try:
        result = runner(['ctest', '--test-dir', build, '-R', f'^{test}$', '-j1',
                         '--output-junit', str(report)])
        run['returncode'] = result.returncode
    except OSError as error:
        run['error'] = exception_message(error)
    finally:
        elapsed = clock() - started
        if math.isfinite(elapsed) and elapsed >= 0:
            run['wall_seconds'] = elapsed
        else:
            run['error'] = 'invalid monotonic clock duration'
    if run['error']:
        return run
    try:
        run['status'], run['error'] = report_status(report, test)
    except Exception as error:
        run['error'] = exception_message(error)
    if run['returncode'] != 0:
        run['status'] = 'failed'
        run['error'] = f"CTest exited with {run['returncode']}"
    return run


def assemble_result(args, runs, error):
    passed = len(runs) == args.repeat and all(r['status'] == 'passed' for r in runs)
    return dict(schema_version=1, kind='实测' if runs else '未测量', test=args.test, repeat=args.repeat,
                runs=runs, all_passed=passed,
                error=error or ('' if passed else 'one or more runs did not pass'))


def write_result(output, result):
    json.dump(result, output, ensure_ascii=False, allow_nan=False, indent=2)
    output.write('\n')
    output.flush()


def main(argv=None, *, runner=subprocess.run, clock=time.monotonic):
    try:
        args = parse_arguments(argv)
    except SystemExit as error:
        return error.code
    # Exclusive creation also prevents a competing invocation from overwriting data.
    try:
        with Path(args.out).open('x', encoding='utf-8') as output:
            error = (check_registration(args.build_dir, args.test, runner)
                     if args.build_dir.strip() else 'build directory must not be empty')
            runs = []
            if not error:
                with tempfile.TemporaryDirectory(prefix='bench-solve-') as temp:
                    for index in range(1, args.repeat + 1):
                        runs.append(run_once(args.build_dir, args.test, index,
                                             Path(temp) / f'run-{index}.xml', runner, clock))
            result = assemble_result(args, runs, error)
            write_result(output, result)
    except OSError as error:
        print(exception_message(error), file=sys.stderr)
        return 2
    return 0 if result['all_passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
