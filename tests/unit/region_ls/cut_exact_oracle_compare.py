"""Public-output acceptance against independent Fraction geometry (standard library only)."""
import argparse
from collections import Counter
from decimal import Decimal, localcontext
from fractions import Fraction as F
from itertools import product
import json
import math
from pathlib import Path
import resource
import subprocess
import sys
import time

import cut_exact_oracle as oracle
from cut_exact_oracle_cases import cases, validate_case

TOL = F(1, 10**13)
NORMAL_TOL = F(1, 10**14)


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def _pairs(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, f'duplicate JSON key: {key}')
        result[key] = value
    return result


def read_json(path):
    def nonfinite(token):
        raise AssertionError(f'nonfinite JSON number: {token}')
    with open(path, encoding='utf-8') as stream:
        # Accumulated long doubles must NOT pass through binary64 here.
        return json.load(stream, parse_float=F, parse_constant=nonfinite, object_pairs_hook=_pairs)


def number(value):
    require(type(value) in (int, F), f'finite number required: {value!r}')
    return F(value)


def vector(value, size):
    require(isinstance(value, list) and len(value) == size, f'expected vector[{size}]')
    return tuple(number(v) for v in value)


def integer(value):
    require(type(value) is int, f'integer required: {value!r}')
    return value


def precise(value):
    value = F(value)
    with localcontext() as context:
        context.prec = 24
        return format(Decimal(value.numerator)/Decimal(value.denominator), '.21g')


def validate_dump(data, case, fem):
    require(isinstance(data, dict), 'dump object required')
    require(F(float(number(data['snap_phi']))) == F(case['snap_phi']), 'dump snap_phi differs')
    require(integer(data['quad_degree']) == case['quad_degree'], 'dump quadrature degree differs')
    require(isinstance(data['axes'], list) and len(data['axes']) == 3, 'three axes required')
    axes = []
    for d, raw in enumerate(data['axes']):
        require(isinstance(raw, list) and len(raw) >= 2, f'axis {d}: at least two entries')
        # Axes and inputs are binary64 coordinates, unlike the accumulated sums.
        axis = tuple(F(float(number(x))) for x in raw)
        require(all(a < b for a, b in zip(axis, axis[1:])), f'axis {d}: not strictly increasing')
        axes.append(axis)
    for d, count in [(0, case['nx']), (1, case['ny'])]:
        require(len(axes[d]) == count+1, f'axis {d}: expected {count+1} entries')
        require(axes[d][0] == F(case['box'][d]) and axes[d][-1] == F(case['box'][d+2]),
                f'axis {d}: box endpoints differ')
    require(axes[2][0] == 0, 'z origin differs')
    endpoint = 0.0
    boundaries = [F(endpoint)]
    # Endpoint identity means binary64 cumulative addition, as in the input model.
    for thickness in case['layers']:
        endpoint += thickness
        boundaries.append(F(endpoint))
    require(axes[2][-1] == boundaries[-1] and all(z in axes[2] for z in boundaries),
            f'z layer boundaries differ: {boundaries}')
    require(len(axes[2]) == 1+len(case['layers'])*case['n_elem_min'], 'z cell count differs')
    dims = tuple(len(a)-1 for a in axes)
    total = math.prod(dims)
    require(isinstance(data['kept'], list) and isinstance(data['cut'], list), 'cell arrays required')
    kept, by_cell = {}, {}
    for raw in data['kept']:
        require(isinstance(raw, list) and len(raw) == 4, 'kept identity shape')
        gid, *ijk = map(integer, raw)
        ijk = tuple(ijk)
        require(0 <= gid < total and all(0 <= ijk[d] < dims[d] for d in range(3)),
                f'out-of-range cell identity: {raw}')
        require(gid not in kept and ijk not in by_cell, f'duplicate kept identity: {raw}')
        kept[gid], by_cell[ijk] = ijk, gid
    cut = {}
    moment_keys = {''.join(map(str, p)) for p in oracle.MOMENTS}
    for record in data['cut']:
        require(isinstance(record, dict), 'cut object required')
        gid = integer(record['gid'])
        require(gid in kept and gid not in cut, f'duplicate/unknown cut GID: {gid}')
        for name in ('volfrac', 'poly_vol', 'vq_w_sum'):
            number(record[name])
        for name in ('mom', 'mom_mfem') if fem else ('mom',):
            require(isinstance(record[name], dict) and set(record[name]) == moment_keys,
                    f'{name}: all ten moments required')
            for value in record[name].values():
                number(value)
        require(isinstance(record['sq'], list) and len(record['sq']) == 7, 'seven sq groups required')
        for group, side in zip(record['sq'], [0, 1, 2, 3, 4, 5, -1]):
            vector(group, 6)
            require(integer(group[0]) == side and integer(group[1]) >= 0, 'sq group order/count')
            if group[1] == 0:
                require(all(v == 0 for v in group[2:]), 'empty sq group has nonzero sums')
        for name, width in [('eb', 6), ('bq_n', 3)]:
            require(isinstance(record[name], list), f'{name} array required')
            for value in record[name]:
                vector(value, width)
        require(len(record['eb']) % 3 == 0, 'eb must contain complete triangles')
        vector(record['ext'], 3)
        cut[gid] = record
    return tuple(axes), kept, by_cell, cut


class Errors:
    def __init__(self):
        self.worst = {}
        self.failures = {}

    def compare(self, case, gid, ijk, quantity, actual, expected, normalization, *, normal=False):
        actual, expected, normalization = map(F, (actual, expected, normalization))
        require(normalization > 0, f'{quantity}: positive normalization required')
        error = abs(actual-expected)/normalization
        category = quantity.split('/')[0]
        is_worst = category not in self.worst or error > self.worst[category][0]
        tolerance = NORMAL_TOL if normal else TOL
        is_failure = error > tolerance and (category not in self.failures or error > self.failures[category][0])
        if not is_worst and not is_failure:
            return error
        detail = (f'case={case} gid={gid} ijk={ijk} quantity={quantity} '
                  f'expected={precise(expected)} actual={precise(actual)} '
                  f'normalization={precise(normalization)} error={float(error):.9g}')
        if is_worst:
            self.worst[category] = (error, detail)
        if is_failure:
            self.failures[category] = (error, detail)
        return error

    def report(self):
        for category, (error, detail) in sorted(self.worst.items()):
            print(f'MAX {category}={float(error):.9g} {detail}', flush=True)
        for _, detail in self.failures.values():
            print(f'FAIL {detail}', flush=True)


def parent_moment(axes, ijk, powers):
    result = F(1)
    for axis, cell, degree in zip(axes, ijk, powers):
        lo, hi = axis[cell:cell+2]
        # Integral of |x^degree|, also correct for parents straddling zero.
        if degree % 2 and lo < 0 < hi:
            result *= (abs(lo)**(degree+1)+hi**(degree+1))/(degree+1)
        else:
            result *= abs(oracle.z_monomial(lo, hi, degree))
    return result


def compare_cell(errors, name, gid, ijk, axes, record, truth, fem):
    check = lambda quantity, actual, expected, scale, **kw: errors.compare(
        name, gid, ijk, quantity, actual, expected, scale, **kw)
    volume = truth['parent_volume']
    for key in ('poly_vol', 'volfrac', 'vq_w_sum'):
        check(f'volume/{key}', record[key]*(volume if key != 'poly_vol' else 1),
              truth['volume'], volume)
    for path in ('mom', 'mom_mfem') if fem else ('mom',):
        for powers, expected in truth['mom'].items():
            key = ''.join(map(str, powers))
            check(f'{path}/{key}', record[path][key], expected, parent_moment(axes, ijk, powers))
    measured_volume = record['mom']['000']
    require(measured_volume > 0, f'{name} gid={gid}: nonpositive quadrature volume')
    for d, key in enumerate(('100', '010', '001')):
        check(f'centroid/{d}', record['mom'][key]/measured_volume,
              truth['centroid'][d], truth['h'][d])
    actual_nonzero = {s for s in range(6) if record['sq'][s][2] > 0}
    expected_nonzero = {s for s, area in enumerate(truth['aperture']) if area > 0}
    require(actual_nonzero == expected_nonzero,
            f'{name} gid={gid} ijk={ijk}: aperture sets actual={actual_nonzero} expected={expected_nonzero}')
    for side in range(6):
        _, count, area, *moments = record['sq'][side]
        expected = truth['aperture'][side]
        if not expected:
            require(count == 0 and all(v == 0 for v in record['sq'][side][2:]),
                    f'{name} gid={gid} ijk={ijk} side={side}: zero aperture n_rec={count} sum={area}')
        else:
            require(count > 0 and area > 0, f'{name} gid={gid}: missing aperture records')
            for d in range(3):
                check(f'face_centroid/{side}/{d}', moments[d]/area,
                      truth['face_centroid'][side][d], truth['h'][d])
        check(f'aperture/{side}', area, expected, truth['parent_face'][side])
    max_face = max(truth['parent_face'])
    expected_area = sum((e[0] for e in truth['eb']), F(0))
    check('embedded_area/sq', record['sq'][6][2], expected_area, max_face)
    for d in range(3):
        check(f'closure/{d}', record['ext'][d], truth['ext'][d], max_face)
    normals = [e[2] for e in truth['eb']]
    for path in ('eb', 'bq_n'):
        coverage = set()
        for raw in record[path]:
            normal = raw[3:] if path == 'eb' else raw
            require(normals, f'{name} gid={gid}: unexpected embedded normal')
            index = min(range(len(normals)),
                        key=lambda n: max(abs(normal[d]-normals[n][d]) for d in range(3)))
            coverage.add(index)
            for d in range(3):
                check(f'normal/{path}/{d}', normal[d], normals[index][d], 1, normal=True)
            if path == 'eb':
                # These are TRIANGLE VERTICES, not the face centroid. Verify the
                # supporting line and parent bounds without moving snapped nodes.
                center = truth['eb'][index][1]
                distance = sum((raw[d]-center[d])*normals[index][d] for d in range(3))
                require(abs(distance) <= F(3, 10**14), f'{name} gid={gid}: eb vertex off face')
                require(all(axes[d][ijk[d]]-F(1, 10**15) <= raw[d] <=
                            axes[d][ijk[d]+1]+F(1, 10**15) for d in range(3)),
                        f'{name} gid={gid}: eb vertex outside parent')
        require(coverage == set(range(len(normals))),
                f'{name} gid={gid} ijk={ijk}: missing {path} expected normal coverage')


def snap_difference(a, b):
    """Include full/void cells whose classification changes under snap."""
    def geometry(t):
        if t['cls'] == 'cut':
            return t['volume'], t['aperture']
        if t['cls'] == 'full':
            return t['parent_volume'], t['parent_face']
        return F(0), [F(0)]*6
    av, aa = geometry(a)
    bv, ba = geometry(b)
    return max([abs(av-bv)/a['parent_volume']] +
               [abs(x-y)/p for x, y, p in zip(aa, ba, a['parent_face'])])


def compare_case(exe, case, workdir, fem, errors):
    validate_case(case)
    case_path = workdir / (case['name']+'.case.json')
    output_path = workdir / (case['name']+'.result.json')
    case_path.write_text(json.dumps(case, allow_nan=False)+'\n', encoding='utf-8')
    command = [str(exe), str(case_path), str(output_path)] + (['--mfem'] if fem else [])
    result = subprocess.run(command, check=False)
    if result.returncode == 77:
        require(fem, f'{case["name"]}: unexpected skip for non-MFEM dump')
        return 77
    require(result.returncode == 0, f'{case["name"]}: dump exited {result.returncode}')
    axes, kept, by_cell, cut = validate_dump(read_json(output_path), case, fem)
    ring = case['ring']
    snap = F(case['snap_phi']) if case['truth_mode'] == 'snap' else None
    oracle.check_ambiguity(axes, ring, F(case['snap_phi']))
    dims = tuple(len(a)-1 for a in axes)
    counts, interior = Counter(), Counter()
    delta = F(0)
    for ijk in product(*(range(n) for n in dims)):
        truth = oracle.cell_truth(axes, ring, ijk, snap=snap)
        gid = by_cell.get(ijk)
        actual_class = 'void' if gid is None else 'cut' if gid in cut else 'full'
        require(actual_class == truth['cls'],
                f'{case["name"]} gid={gid} ijk={ijk}: classification expected={truth["cls"]} actual={actual_class}')
        counts[truth['cls']] += 1
        if case['snap_side']:
            delta = max(delta, snap_difference(truth, oracle.cell_truth(axes, ring, ijk, snap=None)))
        if truth['cls'] == 'cut':
            compare_cell(errors, case['name'], gid, ijk, axes, cut[gid], truth, fem)
            if case['interior']:
                vertex = ring[1]
                if all(axes[d][ijk[d]] < F(vertex[d]) < axes[d][ijk[d]+1] for d in range(2)):
                    require(len(truth['eb']) >= 2, f'{case["name"]}: missing two incident slanted edges')
                    interior[ijk[2]] += 1
        if case['family'] == 'C' and ijk[1] == 0:
            require(actual_class == 'void', f'{case["name"]}: lower row must be void')
    require(sum(counts.values()) == math.prod(dims), 'classification coverage')
    if case['family'] == 'C':
        require(not cut and counts['cut'] == 0, 'C must have zero cut cells')
    if case['interior']:
        require(all(interior[k] >= 1 for k in range(dims[2])), f'{case["name"]}: interior coverage {interior}')
        print(f'INTERIOR {case["name"]} per_z={dict(sorted(interior.items()))}', flush=True)
    if case['snap_side']:
        print(f'SNAP_DIFF {case["name"]} normalized={float(delta):.12g}', flush=True)
        require(delta > TOL, f'{case["name"]}: snap did not change geometry by >1e-13')
    print(f'CLASS {case["name"]} {dict(counts)}', flush=True)
    return 0


def measure(start, wall_limit):
    rss = max(resource.getrusage(resource.RUSAGE_SELF).ru_maxrss,
              resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss)
    rss /= 1024**2 if sys.platform == 'darwin' else 1024
    wall = time.monotonic()-start
    print(f'MEASURE wall_s={wall:.6f} rss_mib={rss:.3f}', flush=True)
    require(wall <= wall_limit and rss <= 512, 'oracle resource limit')


def main():
    start = time.monotonic()
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('--dump', required=True, type=Path)
    parser.add_argument('--group', required=True, choices=['levels', 'variants', 'aniso', 'fem'])
    parser.add_argument('--workdir', required=True, type=Path)
    args = parser.parse_args()
    args.workdir.mkdir(parents=True, exist_ok=True)
    errors = Errors()
    try:
        case_failures = []
        for case in cases(args.group):
            try:
                if compare_case(args.dump, case, args.workdir, args.group == 'fem', errors) == 77:
                    return 77
            except AssertionError as failure:
                detail = f'{case["name"]}: {failure}'
                case_failures.append(detail)
                print(f'FAIL {detail}', flush=True)
        require(not errors.failures and not case_failures,
                f'{len(errors.failures)} geometric categories and {len(case_failures)} cases failed')
        return 0
    finally:
        errors.report()
        measure(start, 120 if args.group == 'levels' else 90)


if __name__ == '__main__':
    sys.exit(main())
