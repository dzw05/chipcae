"""Rounded hat and rotated ring occupy material-free layers; h/h2 PV convergence."""
from parts_support import *


def acceptance(d):
    prepare(d)
    rounded_parts(d)
    stack = imported(d, assembly([part('hat', 'silicon'), part('ring', 'underfill')]))
    for layer in stack['layers']:
        if any('polygon' in r['footprint'] for r in layer.get('regions', [])):
            assert 'material' not in layer, layer
    pv_h, _ = solve(d, 'rounded-h', n=56)
    pv_h2, _ = solve(d, 'rounded-h2', n=112)
    assert abs(pv_h2) > 0
    error = abs(pv_h - pv_h2) / abs(pv_h2)
    print('MEASURED rounded relative PV change', error)
    assert error <= .02, error


if __name__ == '__main__':
    main(acceptance, dependency='SP6-B')
