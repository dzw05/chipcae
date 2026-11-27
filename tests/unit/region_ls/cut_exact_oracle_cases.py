"""Synthetic inputs shared with the dump by writing these values as case JSON."""
from copy import deepcopy

SNAP = 1e-14
LEVELS = [(12, 4), (24, 8), (48, 16), (96, 32)]
V = [[0, .02], [.9, 0], [1, .02], [1, .2], [0, .2]]
V_INT = [[0, .02], [.9031, .0047], [1, .02], [1, .2], [0, .2]]


def case(name, ring, nx=24, ny=8, *, mode='snap', thin=False, interior=False):
    return dict(name=f'{name}_{nx}x{ny}', family=name, ring=deepcopy(ring), nx=nx, ny=ny,
                box=[0, 0, 1, .2], layers=[.05, .0005 if thin else .05], n_elem_min=2,
                max_ratio=100, levels=1, snap_phi=SNAP, quad_degree=6,
                truth_mode=mode, snap_side=name in ('E4', 'F_snap', 'G_snap'), interior=interior)


def cases(group):
    levels = [case(name, ring, nx, ny, interior=name == 'V_int')
              for nx, ny in LEVELS for name, ring in [('V', V), ('V_int', V_INT)]]
    variants = [case('A', [[0, .02], [.875, 0], [1, .02], [1, .2], [0, .2]]),
                case('B', [[0, .05], [1, 0], [1, .2], [0, .2]]),
                case('C', [[0, .025], [1, .025], [1, .2], [0, .2]]),
                case('D', [[0, .05], [.5, .025], [1, .05], [1, .2], [0, .2]])]
    for number, delta in enumerate([1e-7, 1e-9, 1e-12, 3e-15], 1):
        variants.append(case(f'E{number}', [[0, .025-delta], [1, .025+delta],
                                            [1, .2], [0, .2]],
                             mode='snap' if number == 4 else 'nosnap'))
    for nx, ny in [(24, 8), (96, 32)]:
        # The proposed F_snap delta=1e-13 puts the RIGHT incident edge at
        # distance 1.58e-14 from (.875,0), inside the forbidden ambiguity band.
        # The brief requires adjusting such inputs: 3.15e-14 places BOTH incident
        # edges below snap/2 and changes a normalized aperture by >1e-13 at 24x8.
        for side, delta, epsilon in [('snap', 3.15e-14, 1e-10), ('nosnap', 1e-12, 1e-9)]:
            variants.append(case(f'F_{side}', [[0, .02], [.875+delta, 0], [1, .02],
                                                [1, .2], [0, .2]], nx, ny, mode=side))
            variants.append(case(f'G_{side}', [[0, .025-epsilon], [1e-6, 0], [1, 0],
                                                [1, .2], [0, .2]], nx, ny, mode=side))
    aniso = [case('V_int_aniso', V_INT, nx, ny, interior=True)
             for nx, ny in [(192, 4), (8, 64)]]
    aniso.append(case('V_int_thin', V_INT, thin=True, interior=True))
    # FEM group was unspecified in the brief: cover resolved V, interior vertex,
    # fine-grid interior vertex and every anisotropic/thin-layer configuration.
    fem = [c for c in levels if (c['nx'], c['ny']) == (24, 8)
           or (c['nx'] == 96 and c['interior'])]+aniso
    return {'levels': levels, 'variants': variants, 'aniso': aniso, 'fem': fem}[group]


def validate_case(value):
    """Fail before invoking C++ for invalid inputs or lost snap-side annotations."""
    import math
    assert isinstance(value['name'], str) and value['name']
    assert value['truth_mode'] in ('snap', 'nosnap')
    assert type(value['snap_side']) is bool and type(value['interior']) is bool
    for key in ('nx', 'ny', 'levels', 'n_elem_min', 'quad_degree'):
        assert type(value[key]) is int and value[key] > 0, key
    assert value['levels'] == 1 and value['n_elem_min'] == 2 and value['quad_degree'] == 6
    assert value['snap_phi'] == SNAP and value['max_ratio'] == 100
    assert len(value['box']) == 4 and value['box'] == [0, 0, 1, .2]
    assert len(value['layers']) == 2 and value['layers'][0] == .05
    assert value['layers'][1] in (.05, .0005)
    assert len(value['ring']) >= 3 and all(len(p) == 2 for p in value['ring'])
    assert all(type(x) in (int, float) and math.isfinite(x)
               for p in value['ring'] for x in p)
    assert all(value['box'][d] <= p[d] <= value['box'][d+2]
               for p in value['ring'] for d in range(2))
    family = value['family']
    nosnap = family in ('E1', 'E2', 'E3', 'F_nosnap', 'G_nosnap')
    assert value['truth_mode'] == ('nosnap' if nosnap else 'snap'), family
    assert value['snap_side'] == (family in ('E4', 'F_snap', 'G_snap')), family
