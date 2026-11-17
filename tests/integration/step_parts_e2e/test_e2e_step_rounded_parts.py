"""Rounded hat and rotated ring occupy material-free layers; h/h2 PV convergence."""
from parts_support import *


def rounded_box(x, y, z, width, height, depth, radius):
    from OCP.BRepAlgoAPI import BRepAlgoAPI_Fuse
    from OCP.BRepPrimAPI import BRepPrimAPI_MakeCylinder
    from OCP.gp import gp_Ax2, gp_Pnt, gp_Dir
    shape = box(x + radius, y, z, width - 2*radius, height, depth)
    shape = BRepAlgoAPI_Fuse(shape, box(x, y + radius, z, width, height - 2*radius, depth)).Shape()
    for cx in (x + radius, x + width - radius):
        for cy in (y + radius, y + height - radius):
            disk = BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(cx, cy, z), gp_Dir(0, 0, 1)), radius, depth).Shape()
            shape = BRepAlgoAPI_Fuse(shape, disk).Shape()
    return shape


def acceptance(d):
    from OCP.BRepAlgoAPI import BRepAlgoAPI_Cut, BRepAlgoAPI_Fuse
    from OCP.BRepBuilderAPI import BRepBuilderAPI_Transform
    from OCP.gp import gp_Trsf, gp_Ax1, gp_Pnt, gp_Dir
    prepare(d)
    walls = BRepAlgoAPI_Cut(rounded_box(6, 6, 0, 44, 32, 1, 2), box(8, 8, 0, 40, 28, 1)).Shape()
    hat = BRepAlgoAPI_Fuse(walls, rounded_box(6, 6, 1, 44, 32, .5, 2)).Shape()
    ring = BRepAlgoAPI_Cut(box(16, 16, 0, 24, 12, 1), box(18, 18, 0, 20, 8, 1)).Shape()
    rotation = gp_Trsf()
    rotation.SetRotation(gp_Ax1(gp_Pnt(28, 22, 0), gp_Dir(0, 0, 1)), math.radians(15))
    ring = BRepBuilderAPI_Transform(ring, rotation, True).Shape()
    write_step(d / 'hat.step', hat)
    write_step(d / 'ring.step', ring)
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
