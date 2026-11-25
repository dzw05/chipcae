"""Independent exact geometry, adapted from the cutvol audit's verify.py and
cut-oracle-snapcheck's snapcheck.py (audit baseline a16304cb).

Sutherland & Hodgman (1974) clipping; simplex formula
integral_T lambda1^i lambda2^j lambda3^k = 2|T| i!j!k!/(i+j+k+2)!;
z uses prism extrusion. Stroud (1971): degree-six quadrature must integrate
our degree-zero through degree-two polynomials to roundoff.

All coordinates, clipping, squared distances and polynomial integrals are
Fractions. Irrational embedded lengths are evaluated only at the final step
with 60-digit Decimal square roots; final unit normals use math.hypot.
No chipcae imports or actual-result fixtures are used as truth.
"""
from decimal import Decimal, localcontext
from fractions import Fraction as F
from functools import lru_cache
from math import factorial, hypot

MOMENTS = ((0, 0, 0), (1, 0, 0), (0, 1, 0), (0, 0, 1), (2, 0, 0),
           (1, 1, 0), (1, 0, 1), (0, 2, 0), (0, 1, 1), (0, 0, 2))


def snap_phi(f, n2, snap):
    return F(0) if snap is not None and f*f <= snap*snap*n2 else f


def ambiguous(f, n2, snap):
    return snap*snap*n2/4 < f*f < 4*snap*snap*n2


def _clip_tagged(poly, a, b, c, snap, tag):
    # (point, incoming-edge source). A new cap inherits the cutting plane,
    # including when its endpoints were snapped and lie off the exact line.
    if not poly:
        return []
    values = [snap_phi(a*p[0]+b*p[1]-c, a*a+b*b, snap) for p, _ in poly]
    if not any(f > 0 for f in values):
        return poly
    if not any(f < 0 for f in values):
        return []
    out = []

    def append(p, source):
        if not out or out[-1][0] != p:
            out.append((p, source))

    for q, (point, source) in enumerate(poly):
        previous = poly[q-1][0]
        fp, fq = values[q-1], values[q]
        if fp > 0 and fq <= 0:
            t = fp/(fp-fq)
            cross = tuple(previous[d]+t*(point[d]-previous[d]) for d in range(2))
            append(cross, tag)
        if fp <= 0 and fq > 0:
            t = fp/(fp-fq)
            cross = tuple(previous[d]+t*(point[d]-previous[d]) for d in range(2))
            append(cross, source)
        if fq <= 0:
            append(point, source)
    if len(out) > 1 and out[0][0] == out[-1][0]:
        out[0] = (out[0][0], out[-1][1])
        out.pop()
    return out


def clip_halfplane(poly, a, b, c, *, snap=None):
    tagged = [(tuple(map(F, p)), None) for p in poly]
    return [p for p, _ in _clip_tagged(tagged, F(a), F(b), F(c), snap, None)]


def tri_monomial(T, i, j):
    T = [tuple(map(F, p)) for p in T]
    (x1, y1), (x2, y2), (x3, y3) = T
    a2 = abs((x2-x1)*(y3-y1)-(x3-x1)*(y2-y1))
    total = F(0)
    for a1 in range(i+1):
        for a2x in range(i-a1+1):
            a3 = i-a1-a2x
            cx = (F(factorial(i), factorial(a1)*factorial(a2x)*factorial(a3))
                  * x1**a1*x2**a2x*x3**a3)
            if not cx:
                continue
            for b1 in range(j+1):
                for b2 in range(j-b1+1):
                    b3 = j-b1-b2
                    cy = (F(factorial(j), factorial(b1)*factorial(b2)*factorial(b3))
                          * y1**b1*y2**b2*y3**b3)
                    p, q, r = a1+b1, a2x+b2, a3+b3
                    total += cx*cy*F(factorial(p)*factorial(q)*factorial(r),
                                     factorial(p+q+r+2))
    return total*a2


def poly_monomial(poly, i, j):
    return sum((tri_monomial((poly[0], poly[k], poly[k+1]), i, j)
                for k in range(1, len(poly)-1)), F(0))


def _area(poly):
    return sum((p[0]*poly[(i+1) % len(poly)][1]
                - p[1]*poly[(i+1) % len(poly)][0] for i, p in enumerate(poly)), F(0))/2


@lru_cache(maxsize=128)
def _planes(ring):
    ring = list(ring)
    if len(ring) > 1 and ring[0] == ring[-1]:
        ring.pop()
    if len(ring) < 3 or not _area(ring):
        raise ValueError('nondegenerate convex ring required')
    if _area(ring) < 0:
        ring.reverse()
    start = min(range(len(ring)), key=lambda i: (ring[i][1], ring[i][0]))
    ring = ring[start:]+ring[:start]
    planes = []
    for i, p in enumerate(ring):
        q = ring[(i+1) % len(ring)]
        a, b = q[1]-p[1], p[0]-q[0]
        c = a*p[0]+b*p[1]
        if not a*a+b*b or any(a*v[0]+b*v[1] > c for v in ring):
            raise ValueError('convex ring required')
        planes.append((a, b, c))
    return tuple(planes)


def _ring_key(ring):
    return tuple(tuple(map(F, p)) for p in ring)


def _rectangle(x0, x1, y0, y1):
    # Incoming sides: xmin, ymin, xmax, ymax.
    return [((x0, y0), 0), ((x1, y0), 2), ((x1, y1), 1), ((x0, y1), 3)]


def _interval(planes, axis, const, lo, hi):
    """Audit seg_in_poly: clip a scalar interval, independently of polygon clipping."""
    for a, b, c in planes:
        coef, rest = (b, a*const) if axis == 0 else (a, b*const)
        if not coef:
            if rest > c:
                return None
        elif coef > 0:
            hi = min(hi, (c-rest)/coef)
        else:
            lo = max(lo, (c-rest)/coef)
    return (lo, hi) if hi > lo else None


def _edge_rectangle(p, q, x0, x1, y0, y1):
    """Audit embedded-edge clipping, in a separate segment parameter interval."""
    lo, hi = F(0), F(1)
    for d, low, high in [(0, x0, x1), (1, y0, y1)]:
        delta = q[d]-p[d]
        if not delta:
            if not low <= p[d] <= high:
                return None
        else:
            u, v = (low-p[d])/delta, (high-p[d])/delta
            lo, hi = max(lo, min(u, v)), min(hi, max(u, v))
    if hi <= lo:
        return None
    return tuple(tuple(p[d]+t*(q[d]-p[d]) for d in range(2)) for t in (lo, hi))


def _check_unsnapped(poly, planes, bounds):
    """Cross-check all four parent faces and every cap without tagged clipping."""
    x0, x1, y0, y1 = bounds
    for side, const, lo, hi in [(0, x0, y0, y1), (1, x1, y0, y1),
                                 (2, y0, x0, x1), (3, y1, x0, x1)]:
        interval = _interval(planes, side//2, const, lo, hi)
        expected = interval[1]-interval[0] if interval else F(0)
        actual = sum((abs(q[1-side//2]-poly[i-1][0][1-side//2])
                      for i, (q, tag) in enumerate(poly) if tag == side), F(0))
        assert actual == expected, ('oracle interval aperture', side, actual, expected)
    # Consecutive plane intersections recover the canonical outline vertices.
    for index, (a, b, c) in enumerate(planes):
        def intersect(other):
            aa, bb, cc = other
            det = a*bb-aa*b
            if not det:
                return None
            return ((c*bb-cc*b)/det, (a*cc-aa*c)/det)
        p, q = intersect(planes[index-1]), intersect(planes[(index+1) % len(planes)])
        if p is None or q is None:
            continue  # Collinear outline edges need no extra check.
        segment = _edge_rectangle(p, q, *bounds)
        on_parent = (not a and c/b in (y0, y1)) or (not b and c/a in (x0, x1))
        expected = None if on_parent else segment
        actual = [(poly[i-1][0], v) for i, (v, tag) in enumerate(poly)
                  if tag == ('eb', index) and poly[i-1][0] != v]
        assert len(actual) == int(expected is not None), ('oracle embedded count', index)
        if expected:
            assert set(actual[0]) == set(expected), ('oracle embedded endpoints', index)


@lru_cache(maxsize=32768)
def _column(x0, x1, y0, y1, planes, snap):
    original = _rectangle(x0, x1, y0, y1)
    state = 'full'
    # Classification inspects ORIGINAL corners, stopping at the first change.
    for a, b, c in planes:
        phi = [snap_phi(a*p[0]+b*p[1]-c, a*a+b*b, snap) for p, _ in original]
        if any(f > 0 for f in phi):
            state = 'cut' if any(f < 0 for f in phi) else 'void'
            break
    if state != 'cut':
        return state, (), {}
    poly = original
    for i, (a, b, c) in enumerate(planes):
        poly = _clip_tagged(poly, a, b, c, snap, ('eb', i))
    points = [p for p, _ in poly]
    if len(points) < 3 or not _area(points):
        return 'void', (), {}
    if snap is None:
        _check_unsnapped(poly, planes, (x0, x1, y0, y1))
    moments = {(a, b): poly_monomial(points, a, b) for a in range(3) for b in range(3-a)}
    return 'cut', tuple(poly), moments


def _sqrt_fraction(value):
    with localcontext() as ctx:
        ctx.prec = 60
        return F((Decimal(value.numerator)/Decimal(value.denominator)).sqrt())


def z_monomial(lo, hi, degree):
    return (hi**(degree+1)-lo**(degree+1))/(degree+1)


def cell_truth(axes, ring, cell_ijk, *, snap):
    axes = [tuple(map(F, axis)) for axis in axes]
    planes = _planes(_ring_key(ring))
    lo = tuple(axes[d][cell_ijk[d]] for d in range(3))
    hi = tuple(axes[d][cell_ijk[d]+1] for d in range(3))
    h = tuple(hi[d]-lo[d] for d in range(3))
    hx, hy, hz = h
    parent_faces = [hy*hz]*2+[hx*hz]*2+[hx*hy]*2
    cls, poly, xy_mom = _column(lo[0], hi[0], lo[1], hi[1], planes, snap)
    truth = dict(cls=cls, parent_volume=hx*hy*hz, parent_face=parent_faces, h=h)
    if cls != 'cut':
        return truth
    moments = {(a, b, c): xy_mom[a, b]*z_monomial(lo[2], hi[2], c)
               for a, b, c in MOMENTS}
    volume = moments[0, 0, 0]
    centroid = tuple(moments[tuple(int(d == q) for d in range(3))]/volume for q in range(3))
    aperture = [F(0)]*6
    face_mom = [[F(0)]*3 for _ in range(6)]
    eb, eb_vectors = [], []
    for i, (q, source) in enumerate(poly):
        p = poly[i-1][0]
        if p == q:
            continue
        midpoint = ((p[0]+q[0])/2, (p[1]+q[1])/2, (lo[2]+hi[2])/2)
        if isinstance(source, int):
            length = abs(q[1]-p[1]) if source < 2 else abs(q[0]-p[0])
            area = length*hz
            aperture[source] += area
            for d in range(3):
                face_mom[source][d] += area*midpoint[d]
        else:
            a, b, _ = planes[source[1]]
            length = _sqrt_fraction((q[0]-p[0])**2+(q[1]-p[1])**2)
            area = length*hz
            norm = hypot(float(a), float(b))
            normal = (F(float(a)/norm), F(float(b)/norm), F(0))
            eb.append((area, midpoint, normal))
            eb_vectors.append(tuple(area*n for n in normal))
    # Independently integrate each z cross-section, without dividing volume by hz.
    points = [p for p, _ in poly]
    for side, z in [(4, lo[2]), (5, hi[2])]:
        aperture[side] = poly_monomial(points, 0, 0)
        face_mom[side] = [poly_monomial(points, 1, 0), poly_monomial(points, 0, 1),
                          z*aperture[side]]
    face_centroid = [tuple(v/area for v in moment) if area else None
                     for area, moment in zip(aperture, face_mom)]
    truth.update(volume=volume, mom=moments, aperture=aperture, centroid=centroid,
                 face_centroid=face_centroid, eb=eb,
                 ext=tuple(-sum((v[d] for v in eb_vectors), F(0)) for d in range(3)))
    return truth


def check_ambiguity(axes, ring, snap):
    # Repeat clipping independently of cell_truth and its column cache.
    # Every z parent has identical xy corners and generated intersections: the
    # extruded outline has no z coefficient. One fresh check per xy rectangle
    # therefore covers all z slices without consulting the truth cache.
    ax = [tuple(map(F, axis)) for axis in axes]
    planes = _planes(_ring_key(ring))
    for i in range(len(ax[0])-1):
        for j in range(len(ax[1])-1):
            original = [(ax[0][i], ax[1][j]), (ax[0][i+1], ax[1][j]),
                        (ax[0][i+1], ax[1][j+1]), (ax[0][i], ax[1][j+1])]
            mixed = False
            for a, b, c in planes:
                values = [a*p[0]+b*p[1]-c for p in original]
                if any(ambiguous(f, a*a+b*b, snap) for f in values):
                    raise AssertionError('ambiguous snap case')
                values = [snap_phi(f, a*a+b*b, snap) for f in values]
                if any(f > 0 for f in values):
                    mixed = any(f < 0 for f in values)
                    break
            if not mixed:
                continue
            poly = original
            for a, b, c in planes:
                # Only vertices actually evaluated at this clipping stage.
                for p in poly:
                    if ambiguous(a*p[0]+b*p[1]-c, a*a+b*b, snap):
                        raise AssertionError('ambiguous snap case')
                poly = clip_halfplane(poly, a, b, c, snap=snap)
