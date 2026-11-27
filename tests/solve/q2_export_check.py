"""Independent Q2 sidecar reader and tensor evaluator (no solver basis calls)."""
import json
import math
from pathlib import Path

import numpy as np

EPS = np.finfo(float).eps


class CheckError(AssertionError):
    def __init__(self, code, detail):
        super().__init__(f'{code}: {detail}')
        self.code = code


def require(condition, code, detail):
    if not condition:
        raise CheckError(code, detail)


def read_files(paths):
    documents = [json.loads(Path(path).read_text()) for path in paths]
    return documents


def prepare(documents, strict=True):
    """Validate all structure before any field evaluation; return coordinate maps."""
    seen, elements = set(), []
    for document in documents:
        require(set(document) == {'elements'}, 'KEYS', 'top-level keys')
        previous = None
        for raw in document['elements']:
            require(set(raw) == {'gid', 'points', 'U', 'T'}, 'KEYS', 'element keys')
            gid = raw['gid']
            require(type(gid) is int, 'GID', 'gid must be a JSON integer')
            require(gid not in seen, 'DUP_GID', str(gid))
            seen.add(gid)
            require(previous is None or gid > previous, 'ORDER', 'gid order')
            previous = gid
            require(all(len(raw[key]) == 27 for key in ('points', 'U', 'T')),
                    'COUNT', f'gid={gid}: expected 27 nodes in each field')
            p, u, t = (np.asarray(raw[key], dtype=float) for key in ('points', 'U', 'T'))
            require(p.shape == u.shape == (27, 3) and t.shape == (27,), 'COUNT', 'shape')
            require(all(np.isfinite(v).all() for v in (p, u, t)), 'NONFINITE', str(gid))
            require(len({tuple(x) for x in p}) == 27, 'DUP_NODE', str(gid))
            scale = float(np.max(abs(p)))
            levels = []
            for d in range(3):
                axis = []
                for value in np.unique(p[:, d]):
                    if not axis or abs(value-axis[-1]) > 4*EPS*scale:
                        axis.append(value)
                levels.append(np.asarray(axis))
            require(all(len(v) == 3 for v in levels), 'GRID', str(gid))
            require(all(abs(v[1]-(v[0]+v[2])/2) <= 4*EPS*scale for v in levels),
                    'GRID', 'midpoint')
            indices = [np.argmin(abs(p[:, d, None]-levels[d][None, :]), axis=1)
                       for d in range(3)]
            require(all(np.max(abs(p[:, d]-levels[d][indices[d]])) <= 4*EPS*scale
                        for d in range(3)), 'GRID', 'axis level')
            order = (indices[0]+3*indices[1]+9*indices[2]).tolist()
            require(len(set(order)) == 27, 'DUP_NODE', str(gid))
            if strict:
                require(order == list(range(27)), 'ORDER', f'gid={gid}: q=i+3j+9k')
            values = np.empty((27, 4))
            values[order, :3], values[order, 3] = u, t
            elements.append(dict(gid=gid, p=p, u=u, t=t, levels=levels, values=values))
    return elements


def evaluate(element, points):
    points = np.asarray(points, dtype=float).reshape(-1, 3)
    lo = np.array([v[0] for v in element['levels']])
    hi = np.array([v[2] for v in element['levels']])
    r = (points-lo)/(hi-lo)
    basis = np.stack((2*(r-.5)*(r-1), 4*r*(1-r), 2*r*(r-.5)), axis=2)
    result = np.zeros((len(points), 4))
    for k in range(3):
        for j in range(3):
            for i in range(3):
                result += (basis[:, 0, i]*basis[:, 1, j]*basis[:, 2, k])[:, None] * (
                    element['values'][i+3*j+9*k])
    return result


def clip_rectangle(x0, x1, y0, y1, a=1., b=1., c=1.4):
    polygon = [(x0, y0), (x1, y0), (x1, y1), (x0, y1)]
    result = []
    for p, q in zip(polygon, polygon[1:]+polygon[:1]):
        dp, dq = a*p[0]+b*p[1]-c, a*q[0]+b*q[1]-c
        if dp <= 0:
            result.append(p)
        if (dp < 0) != (dq < 0):
            f = dp/(dp-dq)
            result.append((p[0]+f*(q[0]-p[0]), p[1]+f*(q[1]-p[1])))
    return result


def area_centroid(polygon):
    if len(polygon) < 3:
        return 0., None
    cross = [p[0]*q[1]-q[0]*p[1]
             for p, q in zip(polygon, polygon[1:]+polygon[:1])]
    twice = math.fsum(cross)
    if twice == 0:
        return 0., None
    center = [math.fsum((p[d]+q[d])*v for p, q, v in
                        zip(polygon, polygon[1:]+polygon[:1], cross))/(3*twice)
              for d in range(2)]
    return abs(twice)/2, center


def rigid_matrix(p):
    p = np.asarray(p)
    matrix = np.zeros((len(p), 3, 6))
    matrix[:, :, :3] = np.eye(3)
    x, y, z = p.T
    matrix[:, 0, 4], matrix[:, 0, 5] = z, -y
    matrix[:, 1, 3], matrix[:, 1, 5] = -z, x
    matrix[:, 2, 3], matrix[:, 2, 4] = y, -x
    return matrix.reshape(-1, 6)


def check_values(elements, reference, residual_function, cut=False, thermal=False):
    p = np.concatenate([e['p'] for e in elements])
    u = np.concatenate([e['u'] for e in elements])
    exact = reference(p)
    matrix = rigid_matrix(p)
    fit, _, rank, _ = np.linalg.lstsq(matrix, (u-exact).ravel(), rcond=None)
    require(rank == 6, 'VALUE', 'rigid fit rank')
    node_error = float(np.max(abs((u-exact).ravel()-matrix@fit)))
    cross_error = float(residual_function(p, u-exact))
    require(abs(node_error-cross_error) <= 256*EPS*max(float(np.max(abs(u))), 1e-300),
            'VALUE', 'rigid_residual cross-check')
    umax, tmax = (max(float(np.max(abs(e[key]))) for e in elements) for key in ('u', 't'))
    ulimit = 1e-9*float(np.max(abs(exact)))
    max_u, max_t, fidelity = 0., 0., np.zeros(4)
    cut_count = 0
    for e in elements:
        reconstructed = evaluate(e, e['p'])
        original = np.column_stack((e['u'], e['t']))
        fidelity = np.maximum(fidelity, np.max(abs(reconstructed-original), axis=0))
        require(np.all(abs(reconstructed-original) <= 256*EPS*np.array([umax]*3+[tmax])),
                'VALUE', f'gid={e["gid"]}: nodal reconstruction')
        lo = np.array([v[0] for v in e['levels']])
        hi = np.array([v[-1] for v in e['levels']])
        samples = [*e['p'], (lo+hi)/2]
        if cut:
            area, center = area_centroid(clip_rectangle(lo[0], hi[0], lo[1], hi[1]))
            fraction = area/((hi[0]-lo[0])*(hi[1]-lo[1]))
            if 0 < fraction < 1-32*EPS:
                samples.append([*center, (lo[2]+hi[2])/2])
                cut_count += 1
        samples = np.asarray(samples)
        v = evaluate(e, samples)
        if thermal:
            require(np.count_nonzero(e['u']) == 0, 'VALUE', 'thermal-only U must be zero')
        else:
            delta = v[:, :3]-reference(samples)-(rigid_matrix(samples)@fit).reshape(-1, 3)
            max_u = max(max_u, float(np.max(abs(delta))))
        max_t = max(max_t, float(np.max(abs(v[:, 3]-(300+1000*samples[:, 2])))))
    require(max_u <= ulimit, 'VALUE', f'U residual={max_u:.17g} limit={ulimit:.17g}')
    require(max_t <= 1e-10, 'VALUE', f'T error={max_t:.17g} limit=1e-10')
    if cut:
        require(cut_count == 20, 'COUNT', f'cut cells={cut_count}, expected 20')
    print(f'实测 U_residual={max_u:.17g} U_limit={ulimit:.17g} '
          f'T_error={max_t:.17g} T_limit=1e-10 fidelity={fidelity.tolist()}', flush=True)
    return fit
