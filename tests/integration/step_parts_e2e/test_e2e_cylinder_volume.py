"""Exact polygon cut volume and bounded cylinder chord-discretization error."""
from parts_support import *
from itertools import combinations


def check_volumes(area, height, radius, chord_tol, measurements):
    assert all(math.isfinite(x) and x > 0 for x in (area, height, radius, chord_tol))
    polygon_volume = area * height
    exact = math.pi * radius**2 * height
    bound = 1.1 * 4 * chord_tol / (3 * radius)
    for n, measured in measurements:
        assert math.isfinite(measured) and measured > 0, (n, measured)
        polygon_error = abs(measured - polygon_volume) / polygon_volume
        cylinder_error = abs(exact - measured) / exact
        print('MEASURED cylinder', dict(nx=n, ny=n, volume=measured,
              polygon_volume=polygon_volume, polygon_relative_error=polygon_error,
              cylinder_relative_error=cylinder_error, chord_tol=chord_tol, bound=bound,
              bias='below' if measured < exact else 'above'), flush=True)
        assert polygon_error <= 1e-10, (n, polygon_error)
        assert cylinder_error <= bound, (n, cylinder_error, bound)
    for (na, va), (nb, vb) in combinations(measurements, 2):
        error = abs(va - vb) / va
        print('MEASURED cylinder grid pair', na, nb, error, flush=True)
        assert error <= 1e-12, (na, nb, error)


def acceptance(d):
    prepare(d, board_size=(40, 40))
    cylinder(d)
    stack = imported(d, assembly([part('cylinder', 'underfill')]))
    regions = [(layer, r) for layer in stack['layers'] for r in layer.get('regions', [])
               if r.get('body') == 'cylinder']
    assert len(regions) == 1, regions
    layer, region = regions[0]
    assert 'material' not in layer, layer
    assert math.isclose(layer['thickness'], .001, rel_tol=1e-10, abs_tol=1e-12), layer
    points = region['footprint']['polygon']
    assert len(points) >= 3, points
    area = abs(math.fsum(a[0]*b[1] - b[0]*a[1]
                        for a, b in zip(points, points[1:] + points[:1]))) / 2
    # SP4 reuses import_report.json's actual ImportTolerances. Assembly v1.0.2
    # §3 / continuation 7-10 requires SliceOptions.chord_tol = tau_A.
    report = json.loads((d / 'import_report.json').read_text())
    chord_tol = report['tau_A']
    measurements = []
    for n in (8, 16, 32):
        _, path = solve(d, f'cylinder-{n}', n=n, ny=n, levels=0)
        measurements.append((n, volume(path, 'underfill')))
    check_volumes(area, .001, .010, chord_tol, measurements)


if __name__ == '__main__':
    main(acceptance, dependency='SP6-B')
