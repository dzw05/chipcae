"""Supported box ring and convex octagonal lid: cut cells and h/h2 PV change."""
from parts_support import *
from make_parts import ring_chamfer_parts


def acceptance(d):
    prepare(d)
    ring_chamfer_parts(d)
    stack = imported(d, assembly([part('stiffener', 'underfill'), part('lid', 'silicon')]))
    ring = [r for layer in stack['layers'] for r in layer.get('regions', [])
            if r.get('body') == 'stiffener']
    lid = [(layer, r) for layer in stack['layers'] for r in layer.get('regions', [])
           if r.get('body') == 'lid']
    assert ring and all(not r['footprint'].get('polygon') for r in ring), ring
    assert len(lid) == 1, lid
    layer, region = lid[0]
    assert 'material' not in layer, layer
    assert len(region['footprint']['polygon']) == 8, region
    import h5py
    pvs = []
    for n in (2, 4):
        pv, path = solve(d, f'ring-chamfer-{n}', n=n, ny=n, materials=['underfill', 'silicon'])
        with h5py.File(path) as f:
            assert sum(f['VTKHDF/cut/NumberOfCells'][:]) > 0
        pvs.append(pv)
    pv_h, pv_h2 = pvs
    assert abs(pv_h2) > 0
    error = abs(pv_h - pv_h2) / abs(pv_h2)
    print('MEASURED ring/chamfer relative PV change', error, flush=True)
    assert error <= .02, error


if __name__ == '__main__':
    main(acceptance, dependency='SP6-B')
