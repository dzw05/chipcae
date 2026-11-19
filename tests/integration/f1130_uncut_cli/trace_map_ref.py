"""Test-only exact trace geometry and independent tensor Q2 conduction oracle.

No geometry/solver/conversion implementation is imported. SI coordinates are
rationalized before clipping. Only simple disjoint exteriors with contained,
disjoint holes are supported; ambiguous NonZero topology fails explicitly.
Bottom heat is a discrete variational flux: recover unexported Q2 DOFs with
exported corners pinned, integrate top convection by 3x3 Gauss, and negate the
bottom Dirichlet reactions. Corner exports alone cannot supply raw Q2 gradients.
"""
from dataclasses import dataclass
from fractions import Fraction
from itertools import product
import math
from pathlib import Path
import struct

import numpy as np

COPPER_AREAS_MM2 = {'F.Cu': .307835, 'In1.Cu': 2.25, 'In2.Cu': 3., 'B.Cu': 7.}


def rational(value):
    if isinstance(value, (int, Fraction)):
        return Fraction(value)
    assert math.isfinite(float(value)), 'nonfinite coordinate'
    # Recover the decimal SI grid from double exports, including accumulated z.
    result = Fraction(str(value)).limit_denominator(10**12)
    assert abs(float(result) - float(value)) <= 1e-15, 'unsupported coordinate scale'
    return result


@dataclass(frozen=True)
class PolyLayer:
    scale: Fraction
    rings: tuple


def signed_area(ring):
    return sum((rational(x)*rational(v) - rational(u)*rational(y)
                for (x,y), (u,v) in zip(ring, ring[1:]+ring[:1])), Fraction(0))/2


def read_polyset(path):
    data, pos = Path(path).read_bytes(), 0

    def take(size):
        nonlocal pos
        assert 0 <= size <= len(data)-pos, 'truncated polyset'
        out = data[pos:pos+size]
        pos += size
        return out

    def unpack(fmt):
        return struct.unpack(fmt, take(struct.calcsize(fmt)))

    assert take(8) == b'CCPOLY01', 'polyset magic'
    dbu, count = unpack('<dI')
    assert math.isfinite(dbu) and dbu > 0, 'polyset DBU'
    layers = {}
    for _ in range(count):
        name = take(unpack('<I')[0]).decode('utf-8')
        assert name and name not in layers, 'duplicate/empty polyset layer'
        nr, = unpack('<I')
        assert nr+1 <= (len(data)-pos)//8, 'polyset ring count'
        offsets = unpack(f'<{nr+1}q')
        assert offsets[0] == 0 and offsets[-1] <= (len(data)-pos)//16, 'polyset offsets'
        assert all(b-a >= 3 for a,b in zip(offsets, offsets[1:])), 'polyset short ring'
        points = tuple(struct.iter_unpack('<qq', take(16*offsets[-1])))
        rings = tuple(points[a:b] for a,b in zip(offsets, offsets[1:]))
        validate_rings(rings)
        layers[name] = PolyLayer(rational(dbu), rings)
    assert pos == len(data), 'polyset trailing bytes'
    return dict(sorted(layers.items()))


def cross(a, b, c):
    return (b[0]-a[0])*(c[1]-a[1]) - (b[1]-a[1])*(c[0]-a[0])


def on_segment(p, a, b):
    return cross(a,b,p) == 0 and all(min(a[d],b[d]) <= p[d] <= max(a[d],b[d]) for d in (0,1))


def intersects(a,b,c,d):
    ca, cb, cc, cd = cross(a,b,c), cross(a,b,d), cross(c,d,a), cross(c,d,b)
    return (ca*cb < 0 and cc*cd < 0) or any((
        ca == 0 and on_segment(c,a,b), cb == 0 and on_segment(d,a,b),
        cc == 0 and on_segment(a,c,d), cd == 0 and on_segment(b,c,d)))


def inside(point, ring):
    winding = 0
    for a,b in zip(ring, ring[1:]+ring[:1]):
        assert not on_segment(point,a,b), 'touching ring topology unsupported'
        if a[1] <= point[1] < b[1] and cross(a,b,point) > 0:
            winding += 1
        if b[1] <= point[1] < a[1] and cross(a,b,point) < 0:
            winding -= 1
    return winding != 0


def validate_rings(rings):
    for ring in rings:
        assert len(ring) >= 3 and signed_area(ring) != 0, 'degenerate ring'
        edges = list(zip(ring, ring[1:]+ring[:1]))
        assert len(set(ring)) == len(ring), 'repeated ring vertex'
        for i,(a,b) in enumerate(edges):
            for j,(c,d) in enumerate(edges[:i]):
                if i-j != 1 and (j,i) != (0,len(edges)-1):
                    assert not intersects(a,b,c,d), 'self-intersecting ring'
    for i,ring in enumerate(rings):
        for other in rings[:i]:
            assert not any(intersects(a,b,c,d) for a,b in zip(ring,ring[1:]+ring[:1])
                           for c,d in zip(other,other[1:]+other[:1])), 'overlapping rings'
    outers = [r for r in rings if signed_area(r) > 0]
    holes = [r for r in rings if signed_area(r) < 0]
    for i,ring in enumerate(outers):
        for other in outers[:i]:
            assert not inside(ring[0],other) and not inside(other[0],ring), 'overlapping outer rings'
    for i,hole in enumerate(holes):
        assert sum(inside(hole[0],r) for r in outers) == 1, 'hole without unique exterior ring'
        for other in holes[:i]:
            assert not inside(hole[0],other) and not inside(other[0],hole), 'nested hole rings'


def clip_ring(ring, rectangle):
    points = [tuple(map(rational,p)) for p in ring]
    x0,y0,x1,y1 = map(rational, rectangle)
    assert x0 < x1 and y0 < y1, 'degenerate clipping rectangle'
    for axis,bound,sign in ((0,x0,1),(0,x1,-1),(1,y0,1),(1,y1,-1)):
        out = []
        for a,b in zip(points,points[1:]+points[:1]):
            ain, bin_ = sign*(a[axis]-bound) >= 0, sign*(b[axis]-bound) >= 0
            if ain != bin_:
                t = (bound-a[axis])/(b[axis]-a[axis])
                out.append(tuple(a[d]+t*(b[d]-a[d]) for d in (0,1)))
            if bin_:
                out.append(b)
        points = out
    return tuple(points)


def column_fraction(rings, rectangle):
    rings = [tuple(tuple(map(rational,p)) for p in r) for r in rings]
    validate_rings(rings)
    x0,y0,x1,y1 = map(rational, rectangle)
    area = sum((signed_area(clip_ring(r,rectangle)) for r in rings), Fraction(0))
    fraction = area/((x1-x0)*(y1-y0))
    assert 0 <= fraction <= 1, 'invalid clipped copper fraction'
    return fraction


def morton(i,j):
    return sum(((i >> bit)&1) << (2*bit) | ((j >> bit)&1) << (2*bit+1)
               for bit in range(max(i.bit_length(),j.bit_length())))


def layer_edges(stack):
    edges = [Fraction(0)]
    names = []
    for layer in stack['layers']:
        assert layer['name'] not in names, 'duplicate stack layer'
        names.append(layer['name'])
        thickness = rational(layer['thickness'])
        assert thickness > 0, 'nonpositive layer thickness'
        edges.append(edges[-1]+thickness)
    return edges


def tensor_mesh(stack, nx, ny, z_axis=None):
    x0,y0,x1,y1 = map(rational, stack['outline']['box'])
    assert x0 < x1 and y0 < y1, 'degenerate board rectangle'
    if 'polygon' in stack['outline']:
        outline = [tuple(map(rational,p)) for p in stack['outline']['polygon']]
        assert (len(outline) == 4 and set(outline) == set(product((x0,x1),(y0,y1)))
                and all(sum(a[d] != b[d] for d in (0,1)) == 1
                        for a,b in zip(outline,outline[1:]+outline[:1]))), 'unsupported board outline'
    axes = [[lo+(hi-lo)*Fraction(i,n) for i in range(n+1)]
            for lo,hi,n in ((x0,x1,nx),(y0,y1,ny))]
    axes.append(list(z_axis) if z_axis is not None else layer_edges(stack))
    columns = sorted(product(range(nx),range(ny)), key=lambda ij:morton(*ij))
    edges = layer_edges(stack)
    assert axes[2][0] == edges[0] and axes[2][-1] == edges[-1], 'missing layer extent'
    assert all(a < b for a,b in zip(axes[2],axes[2][1:])), 'invalid z axis'
    assert set(edges).issubset(axes[2]), 'missing layer boundary'
    bounds, ids = [], []
    for i,j in columns:
        for a,b in zip(axes[2],axes[2][1:]):
            lid = next((k+1 for k,(lo,hi) in enumerate(zip(edges,edges[1:])) if lo <= a < b <= hi), None)
            assert lid is not None, 'cell crosses layer boundary'
            bounds.append((axes[0][i],axes[1][j],a,axes[0][i+1],axes[1][j+1],b))
            ids.append(lid)
    vertices = sorted({p for box in bounds for p in product(*[(box[d],box[d+3]) for d in range(3)])})
    return dict(axes=axes, bounds=bounds, layer_ids=np.array(ids),
                gids=np.arange(len(bounds)), vertices=vertices, fields={})


def read_hex_mesh(group, stack, require_fields=True):
    def read(name):
        assert name in group, f'M3 missing output field {name}'
        return group[name][:]

    nps, ncs, nids = [read(n) for n in ('NumberOfPoints','NumberOfCells','NumberOfConnectivityIds')]
    assert len(nps) == len(ncs) == len(nids), 'partition count mismatch'
    assert all(np.issubdtype(a.dtype,np.integer) and np.all(a >= 0) for a in (nps,ncs,nids)), 'partition sizes'
    points, con, offsets, types = [read(n) for n in ('Points','Connectivity','Offsets','Types')]
    assert points.shape == (sum(nps),3) and np.all(np.isfinite(points)), 'invalid hex points'
    assert len(con) == sum(nids) and len(offsets) == sum(ncs)+len(ncs), 'topology length'
    assert len(types) == sum(ncs) and np.all(types == 12), 'non-hex cell type'
    temperatures = read('PointData/T')
    assert temperatures.shape == (len(points),) and np.all(np.isfinite(temperatures)), 'nonfinite temperatures'
    raw_fields = {n:read('CellData/'+n) for n in ('layer_id','cell_kind')}
    if 'CellData/elem_gid' in group:
        raw_fields['elem_gid'] = read('CellData/elem_gid')
    # Coefficient layouts and acceptance semantics: README.md (I1 1.1 consumer expectations).
    for name in ('copper_fraction','conductivity_diag'):
        if require_fields or 'CellData/'+name in group:
            raw_fields[name] = read('CellData/'+name)
    count = int(sum(ncs))
    for name,a in raw_fields.items():
        shape = (count,3) if name == 'conductivity_diag' else (count,)
        assert a.shape == shape, f'invalid {name} shape'
    assert np.all(raw_fields['cell_kind'] == 0), 'nonzero hex cell_kind'
    if 'elem_gid' in raw_fields:
        gids = raw_fields['elem_gid']
        assert np.issubdtype(gids.dtype,np.integer) and len(set(gids)) == count, 'duplicate/invalid elem_gid'
    assert np.issubdtype(raw_fields['layer_id'].dtype,np.integer), 'invalid layer_id type'
    vertex_values, vertex_u, bounds = {}, {}, []
    displacement = read('PointData/U')
    assert displacement.shape == (len(points),3) and np.all(np.isfinite(displacement)), 'invalid displacement'
    pbase = cbase = obase = 0
    for npart,ncell,nconn in zip(nps,ncs,nids,strict=True):
        npart,ncell,nconn = int(npart),int(ncell),int(nconn)
        off = offsets[obase:obase+ncell+1]
        assert np.array_equal(off,np.arange(ncell+1)*8) and nconn == ncell*8, 'invalid hex offsets'
        local = con[cbase:cbase+nconn]
        assert np.issubdtype(local.dtype,np.integer) and np.all((0 <= local)&(local < npart)), 'invalid local connectivity'
        for cell in local.reshape((-1,8)):
            pts = [tuple(map(rational,p)) for p in points[pbase+cell]]
            assert len(set(pts)) == 8, 'malformed eight-corner hex'
            lo,hi = tuple(map(min,zip(*pts))),tuple(map(max,zip(*pts)))
            assert all(a < b for a,b in zip(lo,hi)), 'zero-volume hex'
            assert set(pts) == set(product(*zip(lo,hi))), 'nonrectangular hex'
            # VTK corners must describe an affine, positively oriented hex.
            # Checking only the vertex set would also admit crossed faces.
            edges = [tuple(pts[i][d]-pts[0][d] for d in range(3)) for i in (1,3,4)]
            a,b,c = edges
            determinant = (a[0]*(b[1]*c[2]-b[2]*c[1])
                           - a[1]*(b[0]*c[2]-b[2]*c[0])
                           + a[2]*(b[0]*c[1]-b[1]*c[0]))
            local_keys = ((0,0,0),(1,0,0),(1,1,0),(0,1,0),
                          (0,0,1),(1,0,1),(1,1,1),(0,1,1))
            assert determinant > 0 and pts == [
                tuple(pts[0][d]+sum(key[j]*edges[j][d] for j in range(3))
                      for d in range(3)) for key in local_keys], 'invalid hex connectivity'
            bounds.append(lo+hi)
        for index in range(pbase,pbase+npart):
            p = tuple(map(rational,points[index]))
            t,u = float(temperatures[index]),displacement[index]
            if p in vertex_values:
                assert abs(t-vertex_values[p]) <= 1e-12, 'inconsistent boundary temperature'
                assert np.allclose(u,vertex_u[p],rtol=1e-12,atol=1e-15), 'inconsistent boundary displacement'
            vertex_values[p],vertex_u[p] = t,u
        pbase += npart
        cbase += nconn
        obase += ncell+1
    axes = [sorted({b[d] for b in bounds} | {b[d+3] for b in bounds}) for d in range(3)]
    expected = tensor_mesh(stack,len(axes[0])-1,len(axes[1])-1,axes[2])
    assert axes == expected['axes'], 'unexpected mesh axes/outline'
    if 'elem_gid' in raw_fields:
        order = np.argsort(gids)
        assert np.array_equal(gids[order],expected['gids']), 'incomplete cell coverage'
    else:
        # elem_gid is optional in O1. Identify cells by their exact physical
        # bounds while preserving complete, unique coverage of this uncut case.
        by_bounds = {box:i for i,box in enumerate(bounds)}
        assert (len(by_bounds) == len(bounds) == len(expected['bounds'])
                and set(by_bounds) == set(expected['bounds'])), 'incomplete/duplicate cell coverage'
        order = np.array([by_bounds[box] for box in expected['bounds']],dtype=int)
    aligned = [bounds[i] for i in order]
    assert aligned == expected['bounds'], 'elem_gid/Morton column or z membership mismatch'
    assert np.array_equal(raw_fields['layer_id'][order],expected['layer_ids']), 'layer_id membership'
    assert sorted(vertex_values) == expected['vertices'], 'vertex coverage'
    expected.update(fields={n:a[order].copy() for n,a in raw_fields.items()},
                    temperatures=np.array([vertex_values[p] for p in expected['vertices']]),
                    displacement=np.array([vertex_u[p] for p in expected['vertices']]))
    return expected


def reference_coefficients(mesh, rings, stack):
    cache, fractions = {}, []
    for bounds,lid in zip(mesh['bounds'],mesh['layer_ids'],strict=True):
        layer = stack['layers'][lid-1]
        if layer['kind'] == 'Patterned':
            assert layer['material'] == 'fr4_eq' and layer['copper']['metal'] == 'cu', 'Patterned matrix/metal'
            assert 'fraction' not in layer['copper'], 'obsolete layer fraction'
            name = layer['copper']['polygons']['layer']
            assert name == layer['name'] and name in rings, f'missing exact copper layer {name}'
            rectangle = (bounds[0],bounds[1],bounds[3],bounds[4])
            key = (name,rectangle)
            if key not in cache:
                poly = rings[name]
                scaled = [tuple((x*poly.scale,y*poly.scale) for x,y in ring) for ring in poly.rings]
                cache[key] = column_fraction(scaled,rectangle)
            fractions.append(float(cache[key]))
        else:
            assert layer['kind'] == 'Homogenized' and layer['material'] == 'fr4_eq', 'unsupported non-copper material'
            fractions.append(0.)
    phi = np.array(fractions)
    return phi, mixed_conductivity(phi)


def mixed_conductivity(phi):
    parallel = phi*390+(1-phi)*.3
    normal = 1/(phi/390+(1-phi)/.3)
    return np.column_stack((parallel,parallel,normal))


def basis(x):
    return np.array([2*(x-.5)*(x-1),4*x*(1-x),2*x*(x-.5)]), np.array([4*x-3,4-8*x,4*x-1])


def assemble_q2(mesh, conductivity_diag):
    axes = [[a+(b-a)*Fraction(j,2) for a,b in zip(axis,axis[1:]) for j in (0,1)]+[axis[-1]]
            for axis in mesh['axes']]
    points = list(product(*axes))
    index = {p:i for i,p in enumerate(points)}
    cell_dofs = []
    for box in mesh['bounds']:
        local = [[box[d]+(box[d+3]-box[d])*Fraction(j,2) for j in range(3)] for d in range(3)]
        cell_dofs.append(np.array([index[p] for p in product(*local)]))
    gp,gw = np.polynomial.legendre.leggauss(3)
    gp,gw = (gp+1)/2,gw/2
    mass,stiff = np.zeros((3,3)),np.zeros((3,3))
    integral = np.zeros(3)
    for x,w in zip(gp,gw):
        n,d = basis(x)
        mass += w*np.outer(n,n)
        stiff += w*np.outer(d,d)
        integral += w*n
    endpoint = np.diag([0.,0.,1.])
    top_mass = np.kron(np.kron(mass,mass),endpoint)
    top_integral = np.kron(np.kron(integral,integral),[0.,0.,1.])
    rhs,top_weights = np.zeros(len(points)),np.zeros(len(points))
    rows,cols,values = [],[],[]
    assert np.shape(conductivity_diag) == (len(cell_dofs),3), 'reference conductivity shape'
    for box,k,ids in zip(mesh['bounds'],conductivity_diag,cell_dofs,strict=True):
        lengths = np.array([float(box[d+3]-box[d]) for d in range(3)])
        local = np.zeros((27,27))
        for d in range(3):
            factors = [stiff if q == d else mass for q in range(3)]
            local += np.prod(lengths)*k[d]/lengths[d]**2 * np.kron(np.kron(factors[0],factors[1]),factors[2])
        if box[5] == axes[2][-1]:
            area = lengths[0]*lengths[1]
            local += 1000*area*top_mass
            rhs[ids] += 50000*area*top_integral
            top_weights[ids] += area*top_integral
        rows.extend(np.repeat(ids,27))
        cols.extend(np.tile(ids,27))
        values.extend(local.ravel())
    try:
        from scipy.sparse import coo_matrix
        operator = coo_matrix((values,(rows,cols)),shape=(len(points),len(points))).tocsr()
    except ImportError:
        operator = np.zeros((len(points),len(points)))
        np.add.at(operator,(rows,cols),values)
    dofs = dict(points=points,index=index,cells=cell_dofs,top_weights=top_weights,
                bottom=np.array([i for i,p in enumerate(points) if p[2] == axes[2][0]]))
    return operator,rhs,dofs


def solve_q2(operator, rhs, dof_map, use_scipy=True, corners=None):
    pinned = {int(i):0. for i in dof_map['bottom']}
    for point,temp in (corners or {}).items():
        i = dof_map['index'][point]
        assert math.isfinite(temp), 'nonfinite exported temperature'
        if i in pinned:
            assert abs(temp-298.15) <= 1e-8, 'bottom Dirichlet temperature'
        pinned[i] = temp-298.15
    theta = np.zeros(len(rhs))
    for i,value in pinned.items():
        theta[i] = value
    free = np.array([i for i in range(len(rhs)) if i not in pinned],dtype=int)
    if len(free):
        load = rhs-operator@theta
        if use_scipy and hasattr(operator,'tocsr'):
            from scipy.sparse.linalg import spsolve
            theta[free] = spsolve(operator[free][:,free],load[free])
        else:
            dense = operator.toarray() if hasattr(operator,'toarray') else operator
            theta[free] = np.linalg.solve(dense[np.ix_(free,free)],load[free])
    assert np.all(np.isfinite(theta)), 'nonfinite Q2 solution'
    return theta+298.15,operator@theta-rhs


def corner_values(mesh, temperatures, dofs):
    return {p:float(temperatures[dofs['index'][p]]) for p in mesh['vertices']}


def thermal_checks(mesh, stack, conductivity_diag, temperatures, reactions, dofs):
    qtop = 1000*float(dofs['top_weights']@(348.15-temperatures))
    qbot = -float(np.sum(reactions[dofs['bottom']]))
    x,y,_ = mesh['axes']
    area = float((x[-1]-x[0])*(y[-1]-y[0]))
    rlo = rhi = 1/(1000*area)
    for lid,layer in enumerate(stack['layers'],1):
        indices = np.flatnonzero(mesh['layer_ids'] == lid)
        assert len(indices), f'missing layer {layer["name"]}'
        # Volume weights remove repeated xy counting when a layer has many z cells.
        volumes = np.array([math.prod(float(b[d+3]-b[d]) for d in range(3))
                            for i in indices for b in [mesh['bounds'][i]]])
        kz = conductivity_diag[indices,2]
        weights = volumes/np.sum(volumes)
        t = layer['thickness']
        rlo += t/(float(weights@kz)*area)
        rhi += t*float(weights@(1/kz))/area
    return qtop,qbot,rlo,rhi


def check_solution(mesh, rings, stack, measurements, expected_areas=COPPER_AREAS_MM2):
    checks = measurements.setdefault('acceptance_checks',{})
    phi,k = reference_coefficients(mesh,rings,stack)
    fields = mesh['fields']
    for name in ('copper_fraction','conductivity_diag'):
        assert name in fields, f'M3 missing output field {name}'
        assert np.all(np.isfinite(fields[name])), f'M3 nonfinite {name}'
    fraction = fields['copper_fraction']
    assert np.all((0 <= fraction)&(fraction <= 1)), 'M3 fraction outside [0,1]'
    assert np.all(abs(fraction-phi) <= 1e-12*np.maximum(phi,1)), 'M3 cell copper fraction mismatch'
    names = [l['name'] for l in stack['layers'] if l['kind'] == 'Patterned']
    assert set(names) == set(expected_areas), 'missing/unexpected copper layer'
    areas = {}
    for lid,layer in enumerate(stack['layers'],1):
        indices = np.flatnonzero(mesh['layer_ids'] == lid)
        if layer['kind'] != 'Patterned':
            assert np.all(fraction[indices] == 0), 'Core copper must be zero'
            continue
        values = fraction[indices]
        assert np.any(values > 0) and np.any(values == 0), f'{layer["name"]}: requires copper and copper-free columns'
        volume = np.array([math.prod(float(b[d+3]-b[d]) for d in range(3))
                           for i in indices for b in [mesh['bounds'][i]]])
        area = float(values@volume)/layer['thickness']*1e6
        expected = expected_areas[layer['name']]
        assert abs(area-expected)/max(abs(area),abs(expected),1e-30) <= 1e-12, f'{layer["name"]} copper area {area} != {expected} mm2'
        areas[layer['name']] = area
    measurements['copper_areas_mm2'] = areas
    checks['cell_fractions'] = True
    assert np.max(abs(fields['conductivity_diag']-k)/abs(k)) <= 1e-14, 'M3 conductivity mismatch'
    checks['conductivity'] = True
    operator,rhs,dofs = assemble_q2(mesh,k)
    temperatures,_ = solve_q2(operator,rhs,dofs)
    expected = np.array(list(corner_values(mesh,temperatures,dofs).values()))
    error = float(np.max(abs(mesh['temperatures']-expected)))
    measurements['max_temperature_error_K'] = error
    assert error <= 1e-8, f'M3 max temperature error={error:.17g} K > 1e-8 K'
    corners = dict(zip(mesh['vertices'],mesh['temperatures'],strict=True))
    recovered,reactions = solve_q2(operator,rhs,dofs,corners=corners)
    qtop,qbot,rlo,rhi = thermal_checks(mesh,stack,k,recovered,reactions,dofs)
    relative = abs(qtop-qbot)/max(abs(qtop),abs(qbot),1e-30)
    assert relative <= 1e-9, f'M3 energy mismatch {relative}'
    epsilon = 1e-9*max(abs(50/rlo),abs(50/rhi),abs(qbot),1e-30)
    assert 50/rhi-epsilon <= qbot <= 50/rlo+epsilon, 'M3 Wiener heat-flow bounds'
    measurements.update(Q_top_W=qtop,Q_bot_W=qbot,R_lo_K_W=rlo,R_hi_K_W=rhi,
                        energy_relative_error=relative)
    checks['physics'] = True


def compare_aligned(serial, parallel, rtol):
    assert serial['bounds'] == parallel['bounds'] and serial['vertices'] == parallel['vertices'], 'parallel topology mismatch'
    for name in ('copper_fraction','conductivity_diag'):
        a,b = serial['fields'][name],parallel['fields'][name]
        assert np.all(np.isfinite(a)) and np.all(np.isfinite(b)), f'parallel nonfinite {name}'
        assert np.all(abs(a-b) <= rtol*np.maximum(abs(a),abs(b))), f'parallel {name}'
    for name in ('temperatures','displacement'):
        a,b = serial[name],parallel[name]
        assert np.all(np.isfinite(a)) and np.all(np.isfinite(b)), f'parallel nonfinite {name}'
        assert np.all(abs(a-b) <= rtol*np.maximum(abs(a),abs(b))), f'parallel {name}'


def diagnostics(mesh, rings, stack):
    phi,k = reference_coefficients(mesh,rings,stack)
    operator,rhs,dofs = assemble_q2(mesh,k)
    trace,_ = solve_q2(operator,rhs,dofs)
    averages = phi.copy()
    for lid in set(mesh['layer_ids']):
        indices = np.flatnonzero(mesh['layer_ids'] == lid)
        volumes = [math.prod(float(b[d+3]-b[d]) for d in range(3))
                   for i in indices for b in [mesh['bounds'][i]]]
        averages[indices] = np.average(phi[indices],weights=volumes)
    op,load,mapping = assemble_q2(mesh,mixed_conductivity(averages))
    averaged,_ = solve_q2(op,load,mapping)
    print(f'trace_map_vs_layer_average_dT_max={np.max(abs(trace-averaged)):.17g}',flush=True)
    finer = tensor_mesh(stack,2*(len(mesh['axes'][0])-1),2*(len(mesh['axes'][1])-1),mesh['axes'][2])
    _,coefficients = reference_coefficients(finer,rings,stack)
    op,load,mapping = assemble_q2(finer,coefficients)
    doubled,_ = solve_q2(op,load,mapping)
    print(f'trace_map_nx_double_dT_max={(np.max(doubled)-298.15)-(np.max(trace)-298.15):.17g}',flush=True)
