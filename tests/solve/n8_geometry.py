"""Shared synthetic N8 G4 geometry and input builder for geometry tests."""

from fractions import Fraction
import math
from numbers import Real
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / 'integration/fvm_case_e2e'))
from fvm_case_util import bimaterial_strip, read_json, write_json


N8_G4_BOX: tuple[float, float, float, float] = (0.0, 0.0, 1.0, 0.2)
N8_G4_POLYGON: tuple[tuple[float, float], ...] = (
    (0.0, 0.02), (0.9, 0.0), (1.0, 0.02), (1.0, 0.2), (0.0, 0.2))


def _reject(reason):
    raise ValueError('n8_geometry: ' + reason)


def _cross(a, b, c):
    return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])


def _on_segment(a, b, p):
    return all(min(a[k], b[k]) <= p[k] <= max(a[k], b[k]) for k in (0, 1))


def _intersects(a, b, c, d):
    ab_c, ab_d = _cross(a, b, c), _cross(a, b, d)
    cd_a, cd_b = _cross(c, d, a), _cross(c, d, b)
    if ((ab_c == 0 and _on_segment(a, b, c))
            or (ab_d == 0 and _on_segment(a, b, d))
            or (cd_a == 0 and _on_segment(c, d, a))
            or (cd_b == 0 and _on_segment(c, d, b))):
        return True
    return ab_c * ab_d < 0 and cd_a * cd_b < 0


def check_polygon(points, box=N8_G4_BOX) -> None:
    """Validate a simple counterclockwise polygon using exact binary-float values.

    The order of checks is part of the diagnostic contract. Concave polygons
    are allowed; consecutive collinear edges and nonadjacent contact are not.
    """
    try:
        bounds = tuple(box)
        valid_box = (len(bounds) == 4 and all(isinstance(v, Real) for v in bounds)
                     and all(math.isfinite(v) for v in bounds)
                     and bounds[0] < bounds[2] and bounds[1] < bounds[3])
    except (TypeError, ValueError, OverflowError):
        valid_box = False
    if not valid_box:
        _reject('invalid box')
    try:
        vertices = tuple(tuple(p) for p in points)
    except TypeError:
        _reject('invalid vertex')
    if any(len(p) != 2 or not all(isinstance(v, Real) for v in p) for p in vertices):
        _reject('invalid vertex')
    if len(vertices) < 3:
        _reject('fewer than 3 vertices')
    try:
        finite = all(math.isfinite(v) for p in vertices for v in p)
    except OverflowError:
        finite = False
    if not finite:
        _reject('non-finite')

    vertices = tuple(tuple(Fraction(float(v)) for v in p) for p in vertices)
    xmin, ymin, xmax, ymax = (Fraction(float(v)) for v in bounds)
    if len(set(vertices)) != len(vertices):
        _reject('repeated vertex')
    if any(not (xmin <= x <= xmax and ymin <= y <= ymax) for x, y in vertices):
        _reject('outside box')
    n = len(vertices)
    if any(_cross(vertices[i - 1], vertices[i], vertices[(i + 1) % n]) == 0
           for i in range(n)):
        _reject('collinear vertex')
    for i in range(n):
        for j in range(i + 1, n):
            if j == i + 1 or (i == 0 and j == n - 1):
                continue
            if _intersects(vertices[i], vertices[(i + 1) % n],
                           vertices[j], vertices[(j + 1) % n]):
                _reject('self-intersecting')
    twice_area = sum(a[0] * b[1] - a[1] * b[0]
                     for a, b in zip(vertices, vertices[1:] + vertices[:1]))
    if twice_area <= 0:
        _reject('clockwise')


def n8_g4_case(root: pathlib.Path, nx: int, ny: int, kz: int, family: str = 'fvm') -> dict:
    """Write the four N8 input files, reusing the main bimaterial strip builder."""
    if family not in ('fvm', 'fem'):
        _reject('invalid family')
    for name, value in (('nx', nx), ('ny', ny), ('kz', kz)):
        if isinstance(value, bool) or not isinstance(value, int) or value <= 0:
            _reject(name + ' must be a positive integer')
    check_polygon(N8_G4_POLYGON)
    temporary_profiles = [root / f'profile{ranks}.json' for ranks in (1, 2)
                          if not (root / f'profile{ranks}.json').exists()]
    case = bimaterial_strip(root)
    for path in temporary_profiles:
        path.unlink()
    stack = read_json(root / 'stack.json')
    stack['outline']['polygon'] = [list(p) for p in N8_G4_POLYGON]
    for layer in stack['layers']:
        layer['n_elem_min'] = kz
    case['mesh'].update(nx=nx, ny=ny, min_elems_per_layer=kz, cell_budget=1000000)
    for physics in case['physics']:
        physics['solver'] = family + '.' + physics['solver'].split('.', 1)[1]
    case['solver'] = {}
    write_json(root / 'stack.json', stack)
    write_json(root / 'case.json', case)
    write_json(root / 'profile.json', dict(schema='chipcae.run_profile/1.0',
                                         ranks=1, backend='cpu', gpus=0))
    return case
