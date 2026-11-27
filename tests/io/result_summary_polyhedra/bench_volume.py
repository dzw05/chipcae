# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
"""Manual paired T/M acceptance benchmark; never imported by CTest.

Use the same Python with NumPy/h5py for both repositories. --main accepts a Git
checkout or a read-only archive with its full commit in .benchmark-commit.
Pass --box-baseline 9.739 on the reference box; omit on other machines.
The measuring parent deliberately imports neither NumPy nor h5py. Exit status:
0 = passed (or unchecked), 1 = performance failure, 2 = invalid environment/run.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shlex
import subprocess
import sys
import tempfile
import time


_GENERATE = r'''
from pathlib import Path
import shutil
import sys
import unittest
sys.path.insert(0, str(Path(sys.argv[1]) / 'tests/io/result_summary_polyhedra'))
from polyhedra_fixtures import cube_fixture, h5py
base, n = Path(sys.argv[2]), int(sys.argv[3])
topology, plain = base / 'topology', base / 'plain'
step, _ = cube_fixture(unittest.TestCase(), topology, cells=(n // 2, n - n // 2))
shutil.copytree(topology, plain)
with h5py.File(plain / step.relative_to(topology), 'r+') as stream:
    block = stream['VTKHDF/cut']
    for name in ('FaceConnectivity', 'FaceOffsets', 'PolyhedronToFaces',
                 'PolyhedronOffsets', 'NumberOfFaces', 'NumberOfFaceConnectivityIds',
                 'NumberOfPolyhedronToFaceIds', 'CellData/volume'):
        del block[name]
'''

_STATS = r'''
import json
import sys
import numpy as np
samples = json.load(sys.stdin)
result = {}
for name, values in samples.items():
    q1, median, q3 = np.percentile(values, [25, 50, 75])
    result[name] = dict(median=float(median), q1=float(q1), q3=float(q3),
                        iqr=float(q3 - q1), minimum=float(min(values)),
                        maximum=float(max(values)))
print(json.dumps(result))
'''


def _rss_kib(ru_maxrss: int, platform: str | None = None) -> int:
    # Copied from tools/vv/real_case_e2e.py; do not import that heavy module.
    platform = sys.platform if platform is None else platform
    return -(-ru_maxrss // 1024) if platform == "darwin" else int(ru_maxrss)


def _available_memory():
    """Use platform commands, with no /proc dependency or third-party modules."""
    if sys.platform == 'darwin':
        output = subprocess.check_output(['vm_stat'], text=True)
        page_size = int(re.search(r'page size of (\d+) bytes', output).group(1))
        pages = dict((name.strip(), int(value)) for name, value in
                     re.findall(r'^([^:\n]+):\s+(\d+)\.', output, re.MULTILINE))
        # macOS has no MemAvailable: explicitly record this reclaimable proxy.
        return {'available_bytes': page_size * sum(pages.get(name, 0) for name in
                ('Pages free', 'Pages inactive', 'Pages speculative')),
                'available_metric': 'vm_stat free+inactive+speculative'}
    output = subprocess.check_output(['free', '-b'], text=True).splitlines()
    headers, memory = output[0].split(), output[1].split()[1:]
    return {'available_bytes': int(memory[headers.index('available')]),
            'available_metric': 'free -b available (MemAvailable)'}


def _environment():
    return dict(_available_memory(), load_average=list(os.getloadavg()))


def _commit(root):
    marker = root / '.benchmark-commit'
    value = (marker.read_text().strip() if marker.is_file() else
             subprocess.check_output(['git', '-C', str(root), 'rev-parse', 'HEAD'],
                                     text=True).strip())
    if not re.fullmatch('[0-9a-f]{40}', value):
        raise ValueError(f'invalid commit identity for {root}: {value!r}')
    return value


def _measure(argv):
    # Files avoid a pipe-buffer deadlock; wait4 is the only reaper. Setting
    # returncode prevents Popen's destructor from attempting a second wait.
    with tempfile.TemporaryFile() as out, tempfile.TemporaryFile() as err:
        start = time.monotonic()
        process = subprocess.Popen(argv, stdout=out, stderr=err)
        _, status, usage = os.wait4(process.pid, 0)
        process.returncode = os.waitstatus_to_exitcode(status)
        wall = time.monotonic() - start
        out.seek(0)
        err.seek(0)
        stdout, stderr = out.read(), err.read()
    return dict(wall_s=wall, peak_rss_kib=_rss_kib(usage.ru_maxrss),
                returncode=process.returncode,
                stdout_sha256=hashlib.sha256(stdout).hexdigest(),
                stdout_bytes=len(stdout), stderr_bytes=len(stderr),
                stderr=stderr.decode('utf-8', errors='replace'), argv=argv,
                command=shlex.join(argv))


def _statistics(samples):
    # NumPy's default linear percentiles, isolated from the measuring parent.
    completed = subprocess.run([sys.executable, '-c', _STATS],
                               input=json.dumps(samples), text=True,
                               capture_output=True, check=True)
    return json.loads(completed.stdout)


def _run(args, emit):
    roots = dict(main=args.main.resolve(), branch=args.branch.resolve())
    commits = {name: _commit(root) for name, root in roots.items()}
    initial = _environment()
    emit(dict(event='session', commits=commits,
              repositories={name: str(root) for name, root in roots.items()},
              machine=platform.uname()._asdict(), cpu_count=os.cpu_count(),
              python=sys.executable, python_version=sys.version, environment=initial,
              sizes=args.n, repeat=args.repeat, box_baseline_s=args.box_baseline,
              percentile_method='numpy.percentile default linear'))
    if initial['available_bytes'] < 3_000_000_000:
        emit(dict(event='invalid_environment', reason='available memory below 3 GB'))
        return 2
    performance_failed = environment_invalid = False
    with tempfile.TemporaryDirectory(prefix='rs-poly-volume-') as tmp:
        for n in args.n:
            folder = Path(tmp) / str(n)
            generated = subprocess.run([sys.executable, '-c', _GENERATE,
                                        str(roots['main']), str(folder), str(n)],
                                       capture_output=True, text=True)
            if generated.returncode != 0 or generated.stderr:
                emit(dict(event='invalid_run', stage='generate', n=n,
                          returncode=generated.returncode, stdout=generated.stdout,
                          stderr=generated.stderr))
                return 2
            samples = []
            hashes = {}
            for topology in (True, False):
                fixture = folder / ('topology' if topology else 'plain')
                # Strict M,B,M,B,... for each mode, retaining every sample.
                for repeat in range(args.repeat):
                    for name in ('main', 'branch'):
                        environment = _environment()
                        if environment['available_bytes'] < 3_000_000_000:
                            emit(dict(event='invalid_environment', n=n,
                                      reason='available memory below 3 GB', **environment))
                            return 2
                        argv = [sys.executable, str(roots[name] / 'tools/vv/result_summary.py'),
                                str(fixture)]
                        sample = dict(event='sample', n=n, topology=topology,
                                      repository=name, repeat=repeat, commit=commits[name],
                                      environment=environment, **_measure(argv))
                        emit(sample)
                        samples.append(sample)
                        key = name, topology
                        previous = hashes.setdefault(key, sample['stdout_sha256'])
                        if (sample['returncode'] != 0 or sample['stderr_bytes'] != 0
                                or sample['stdout_bytes'] == 0
                                or sample['stdout_sha256'] != previous):
                            emit(dict(event='invalid_run', n=n,
                                      reason='unsuccessful, noisy or nondeterministic child'))
                            return 2
                if hashes['main', topology] != hashes['branch', topology]:
                    emit(dict(event='invalid_run', n=n, reason='main/branch stdout differ'))
                    return 2
            if hashes['main', True] != hashes['main', False]:
                emit(dict(event='invalid_run', n=n, reason='fixture modes are not equivalent'))
                return 2
            values = {}
            for name in ('main', 'branch'):
                for topology in (True, False):
                    key = name + ('_topology' if topology else '_plain')
                    chosen = [s for s in samples if s['repository'] == name
                              and s['topology'] == topology]
                    values[key] = [s['wall_s'] for s in chosen]
                    values[key + '_rss'] = [s['peak_rss_kib'] for s in chosen]
            stats = _statistics(values)
            measured_t = stats['branch_topology']['median']
            limit_t = 2 * stats['branch_plain']['median'] + 1.5 * stats['branch_plain']['iqr']
            passed_t = measured_t <= limit_t
            main_rss = stats['main_topology_rss']
            measured_m = stats['branch_topology_rss']['maximum']
            limit_m = main_rss['maximum'] + main_rss['maximum'] - main_rss['minimum']
            passed_m = measured_m <= limit_m if n == 50000 else None
            valid = True
            if n == 50000 and args.box_baseline is not None:
                median = stats['main_topology']['median']
                valid = .7 * args.box_baseline <= median <= 1.3 * args.box_baseline
            emit(dict(event='gates', n=n, statistics=stats, environment_valid=valid,
                      T=dict(measured_s=measured_t, limit_s=limit_t, passed=passed_t),
                      M=dict(measured_kib=measured_m, limit_kib=limit_m, passed=passed_m),
                      status=('invalid_environment' if not valid else
                              'pass' if passed_t and passed_m is not False else 'performance_failure')))
            environment_invalid |= not valid
            performance_failed |= not passed_t or passed_m is False
    # Detect checkout movement during the session, independently of performance.
    if {name: _commit(root) for name, root in roots.items()} != commits:
        emit(dict(event='invalid_run', reason='repository commit changed during measurement'))
        return 2
    status = ('invalid_environment' if environment_invalid else
              'performance_failure' if performance_failed else 'pass')
    emit(dict(event='result', status=status, checked=args.check))
    if environment_invalid:
        return 2
    return 1 if args.check and performance_failed else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--main', type=Path, required=True)
    parser.add_argument('--branch', type=Path, required=True)
    parser.add_argument('--n', type=int, nargs='+', default=[10000, 50000])
    parser.add_argument('--repeat', type=int, default=5)
    parser.add_argument('--check', action='store_true')
    parser.add_argument('--out', type=Path, help='also archive the stdout JSONL here')
    parser.add_argument('--box-baseline', type=float,
                        help='reference-box 50k main median (9.739 seconds)')
    args = parser.parse_args()
    if args.repeat < 1 or any(n < 1 for n in args.n):
        parser.error('sizes and repeat must be positive')
    if args.check and (args.repeat != 5 or any(n not in (10000, 50000) for n in args.n)):
        parser.error('--check requires five repeats and acceptance sizes 10000/50000')
    if args.box_baseline is not None and args.box_baseline <= 0:
        parser.error('--box-baseline must be positive')
    archive = args.out.open('w', encoding='utf-8') if args.out else None

    def emit(record):
        line = json.dumps(record, sort_keys=True, allow_nan=False)
        print(line, flush=True)
        if archive is not None:
            archive.write(line + '\n')
            archive.flush()

    try:
        return _run(args, emit)
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        emit(dict(event='invalid_run', reason=str(error)))
        return 2
    finally:
        if archive is not None:
            archive.close()


if __name__ == '__main__':
    raise SystemExit(main())
