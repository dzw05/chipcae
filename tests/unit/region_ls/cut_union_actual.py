"""Run the frozen union observer/checker, then enforce absolute fraction error <=1e-13."""
import argparse
from fractions import Fraction as F
import json
from pathlib import Path
import resource
import subprocess
import sys
import time

from cut_exact_oracle_compare import integer, number, read_json, require


def check_fractions(actual, fixture_root):
    paths = sorted(fixture_root.glob('*/expected.json'))
    require(len(paths) == 14, f'expected all 14 union fixtures, found {len(paths)}')
    names = {p.parent.name for p in paths}
    require(set(actual) == names | {'O8_bitwise'}, 'union case set differs')
    worst, detail, cells = F(-1), '', 0
    for path in paths:
        expected = read_json(path)
        name = path.parent.name
        result = actual[name]
        require(result['ok'] is expected['ok'], f'{name}: status differs')
        if not expected['ok']:
            continue
        require(len(expected['cells']) == len(expected['cells_exact']), f'{name}: fixture shape')
        truth = {tuple(row[:3]): F(exact)
                 for row, exact in zip(expected['cells'], expected['cells_exact'])}
        measured = {}
        for row in result['cells']:
            require(isinstance(row, list) and len(row) == 4, f'{name}: cell shape')
            key = tuple(integer(i) for i in row[:3])
            require(key not in measured, f'{name}: duplicate cell {key}')
            measured[key] = number(row[3])
        require(set(measured) == set(truth), f'{name}: fraction cell keys differ')
        for key, want in truth.items():
            error = abs(measured[key]-want)
            cells += 1
            if error > worst:
                worst = error
                detail = (f'case={name} ijk={key} quantity=sub_volfrac '
                          f'expected={float(want):.21g} actual={float(measured[key]):.21g} '
                          f'normalization=1 error={float(error):.9g}')
        print(f'UNION {name} cells={len(truth)}', flush=True)
    print(f'MAX union_fraction={float(worst):.9g} {detail} cells={cells}', flush=True)
    require(worst <= F(1, 10**13), f'union fraction abs 1e-13: {detail}')


def main():
    start = time.monotonic()
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('--dump', required=True, type=Path)
    parser.add_argument('--checker', required=True, type=Path)
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    args.out.parent.mkdir(parents=True, exist_ok=True)
    try:
        with args.out.open('w', encoding='utf-8') as stream:
            result = subprocess.run([str(args.dump)], stdout=stream, check=False)
        if result.returncode:
            return result.returncode
        result = subprocess.run([sys.executable, str(args.checker), '--actual', str(args.out)], check=False)
        if result.returncode:
            return result.returncode
        check_fractions(read_json(args.out), args.checker.parent)
        return 0
    finally:
        rss = max(resource.getrusage(resource.RUSAGE_SELF).ru_maxrss,
                  resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss)
        rss /= 1024**2 if sys.platform == 'darwin' else 1024
        wall = time.monotonic()-start
        print(f'MEASURE wall_s={wall:.6f} rss_mib={rss:.3f}', flush=True)
        require(wall <= 30 and rss <= 512, 'union resource limit')


if __name__ == '__main__':
    sys.exit(main())
