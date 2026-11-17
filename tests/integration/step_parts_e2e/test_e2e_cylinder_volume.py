"""Cylinder cut-volume convergence, analytic SI volume, least-squares order >=1.8.

The referenced mesh sequence is absent from the supplied brief: use explicit
h = 2, 1, .5 mm (56 mm outline / nx = 28, 56, 112), pending clarification.
"""
from parts_support import *


def acceptance(d):
    from OCP.BRepPrimAPI import BRepPrimAPI_MakeCylinder
    from OCP.gp import gp_Ax2, gp_Pnt, gp_Dir
    prepare(d)
    write_step(d / 'cylinder.step', BRepPrimAPI_MakeCylinder(
        gp_Ax2(gp_Pnt(28, 22, 0), gp_Dir(0, 0, 1)), 10, 1).Shape())
    imported(d, assembly([part('cylinder', 'underfill')]))
    exact = math.pi * .010**2 * .001
    hs, errors = [], []
    for n in (28, 56, 112):
        _, path = solve(d, f'cylinder-{n}', n=n)
        measured = volume(path, 'underfill')
        error = abs(measured - exact) / exact
        assert 0 < error < 1, (n, measured, exact, error)
        hs.append(math.log(.056/n))
        errors.append(math.log(error))
        print('MEASURED cylinder', .056/n, measured, error)
    xmean, ymean = sum(hs)/len(hs), sum(errors)/len(errors)
    slope = sum((x-xmean)*(y-ymean) for x, y in zip(hs, errors)) / sum((x-xmean)**2 for x in hs)
    print('MEASURED cylinder least-squares order', slope)
    assert slope >= 1.8, slope


if __name__ == '__main__':
    main(acceptance, dependency='SP6-B')
