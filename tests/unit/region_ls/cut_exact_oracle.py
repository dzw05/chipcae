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
from itertools import product
from math import factorial, hypot, isqrt

MOMENTS = ((0, 0, 0), (1, 0, 0), (0, 1, 0), (0, 0, 1), (2, 0, 0),
           (1, 1, 0), (1, 0, 1), (0, 2, 0), (0, 1, 1), (0, 0, 2))

# Derivation required by the revised brief (Higham 2002 §§2.2, 3.1).
# Adaptation provenance: audit gateU.py and rule_replica.py, 2026-11-26.
# （首席 06:33 / 06:40 / 06:52 裁定；rulings/test-cut-exact-oracle-sliver-centroid.md、rulings/test-cut-exact-oracle-gateA.md）
#
# 误差模型：Higham, *Accuracy and Stability of Numerical Algorithms*, 2nd ed., SIAM 2002, §2.2（fl(x op y) = (x op y)(1+δ)，|δ| ≤ u = 2⁻⁵³，含 √）与 §3.1（∏(1+δ_i)^{±1} = 1+θ_n，|θ_n| ≤ γ_n = nu/(1−nu)）。下面的推导必须原样写进 `cut_exact_oracle.py` 的注释和 PR 描述（逐步列运算与 θ 计数）。常数只能由推导得出，不得用实测值反推；若推出的界盖不住实测，测试保持红，停下报 mesh/首席，不得调常数。
#
# ### U：交点绝对误差界 |v − r| ≤ c·u·S_v，c = 47（独立硬断言；原「≤1 ulp」作废）
#
# 实现路径（本卡算例全是凸环，走 `apply_outline_cut` → 规范化环（面积 <0 则反转，起点取 (y,x) 最小）→ 每条边一个平面 → `generate()` 再归一 → `clip()`；平面顺序与 oracle `_planes` 相同）。逐步计数（u 为 double 单位舍入；long double 也按 u 计，覆盖 macOS arm64 long double = double）：
# 1. 边 a→b（a、b 是环顶点 double）：nx0 = fl(b_y − a_y)，ny0 = fl(a_x − b_x)，各 (1+δ)；L0 = `std::hypot(nx0, ny0)`。假设：libm hypot 误差 ≤ 1 ulp（≤ 2u 相对；glibc 手册「Known Maximum Errors in Math Functions」x86_64 hypot = 1 ulp），故 L0 = |N|(1+θ_3)，N = (b_y − a_y, a_x − b_x) 精确。nx1 = fl(nx0/L0) = ñ_x(1+θ_5)（ñ = N/|N|）；d1 = fl(fl(nx1·a_x) + fl(ny1·a_y))，两项各 (1+θ_7)。
# 2. `generate()`：s = fl(fl(nx1²) + fl(ny1²)) + 0（n_z = 0，加 0 精确）= 1+θ_12；L1 = fl(√s) = 1+θ_13（√(1+θ_12) 仍是 12 个 (1+δ')^{±1} 之积，|δ'| ≤ u）；inv = fl(1/L1)；n2 = fl(n1·inv) = ñ(1+θ_20)；d2 = fl(d1/L1)，两项各 (1+θ_21)。
# 3. φ̂(p) = fl(fl(fl(n2_x p_x) + fl(n2_y p_y)) + fl(0·p_z)) − d2 = ñ_x p_x(1+θ_23) + ñ_y p_y(1+θ_23) − ñ_x a_x(1+θ_22) − ñ_y a_y(1+θ_22)。故 |φ̂(p) − φ̃(p)| ≤ γ_23·S_φ(p)，S_φ(p) = |ñ_x|(|p_x| + |a_x|) + |ñ_y|(|p_y| + |a_y|)，φ̃ 为到精确平面的有向距离。|p| 用该边两端精确值加其自身误差界取上界。
# 4. 穿越（pa·pb < 0，`pa − pb` 无相消）：t̂ = fl(pa/fl(pa − pb)) = t̃(1+θ_2)，t̃ = pa/(pa − pb) ∈ [0,1]；|t̃ − t_e| ≤ (|e_a| + |e_b|)/|pa − pb| ≤ 2γ_23 S_φ/(G(1−ρ))，G = |φ̃_a − φ̃_b|，ρ = 2γ_23 S_φ/G（要求 ρ < 1，否则只用下面的线段界）。
# 5. v_k = fl(a_k + fl(fl(b_k − a_k)·t̂))；边两端某坐标相同则该坐标 b_j − a_j = 0，v_j = a_j 精确复制。
# - 情形 L（边在单元边界线上，自由坐标 k，Δ = |b_k − a_k|）：两端都在同一条网格线上，平面与「过计算端点的直线」的精确交点就是 v*，端点沿线的误差不进入。G = |ñ_k|Δ，|v_k − v*_k| ≤ (1+u)[Δ_hi·2γ_23 S_φ/(|ñ_k|Δ_lo(1−ρ)) + γ_4Δ_hi] + u|v*_k|；再加 |r − v*| ≤ u|v*|（r = v* 的 round-to-nearest）。因 2γ_23(1+u)/u < 46.0001、γ_4(1+u)/u < 4.0001，取 **c = 47**，**S_v = S_φ·Δ_hi/(|ñ_k|·Δ_lo·(1−ρ)) + Δ_hi + |v*_k|**（Δ_lo/hi = 精确端点差 ∓ 两端误差界；比值里 |N| 约掉，全部 Fraction 计算）。v 与 v* 都在该线段上，所以另有 S_v ≤ (Δ_hi + E_a + E_b)/(c·u) + |v*_k|，取两者小者。
# - 情形 S（斜边，两端都是计算点，如 V_int 单元内顶点）：s = 边与平面夹角正弦的下界（(|ñ·(q*−p*)| − 2E_ab)/(|q*−p*| + 2E_ab)，√ 用带余量的上下界），E_ab = 端点误差界（2-范数）。端点误差使直线移动 ≤ E_ab，交点沿新线移动 ≤ E_ab/s，故 **S_v,j = S_ab(1 + 1/s) + S_φ·Δ_hi,j/(s·ℓ_lo·(1−ρ)) + Δ_hi,j + |v*_j|**（S_ab 为端点尺度和，ℓ_lo 为边长下界），同样 c = 47，同样取线段界的小者。
# - FMA 收缩只减少舍入次数，上面的界仍成立；端点 snap 为 φ = 0 时实现取端点本身（复制，继承端点的界），oracle 的 snap 语义相同。
# - 断言（驱动，逐切割单元）：oracle 按实现的平面顺序做带界的 Sutherland–Hodgman（`clip_bounds`），每个顶点带 (精确值 v*, S_v, 固定坐标标记)，结果顶点集必须与 `_column` 的精确多边形完全相同。对 dump 的每个 `eb` 顶点 (x,y)：必须匹配某个精确顶点，固定坐标精确相等，自由坐标 |v − v*| ≤ c·u·S_v 且 |v − r| ≤ c·u·S_v；每个非角点精确顶点至少有一个 eb 顶点落在它的盒里。超界即红，打印 max |v − v*|/(u·S_v)。
# - mesh 07:07 诊断实测（gateU.py，5600 切割单元、16008 个 eb 顶点，L 型 2845、S 型 12）：0 失败，max |v − v*|/(u·S_v) = 0.868（V_int_48x16），c = 47。
#
# ### 门 A：区间参考（形心与面形心），门值 1e-13·h 不变
#
# - 盒：每个精确顶点的每个自由坐标取 [v* − c·u·S_v, v* + c·u·S_v]；落在单元边界线上的坐标固定为精确值。参考集 = 顶点在盒内任取时、用 Fraction 精确算出的形心集合；不得读实现顶点或实现输出当期望值。
# - 体形心与 z 面（4/5）形心：xy 截面多边形（棱柱模型）在盒上的形心集合；体形心 z 分量与 z 面 z 分量精确固定。
# - 侧面（0–3）形心：实现对同一交点在 z0、z1 两条边上各算一份（诊断：G 上下两份差 1 ulp），所以侧面按 (s, z) 梯形建模，下边端点（z0）与上边端点（z1）各自独立取盒内值；固定法向坐标精确。
# - 判定（自适应，首席 06:52 同意）：① 盒角点枚举得到的取值区间（这些值都是参考集里真实取到的值）；实现值到它的距离 ≤ 容差 → 过。② 否则算严格外包：均值形式 f(m) + Σ G_i·[−r_i, r_i]，G_i 是 Fraction 区间前向自动微分在整盒上对 ∂f/∂x_i 的包络；到外包距离 > 容差 → 红。③ 夹在中间才二分最宽的坐标，至多 10 层；仍无法判定 → 红（不放过）。
# - 容差：体形心 1e-13·h_axis；面形心 1e-13·h_axis + Q3（下条），全部单元都查，不按占比豁免。
# - 诊断实测：100740 项 0 失败；扣除 Q3 后到角点区间的最大距离 9.72e-15·h；没有一项需要二分。
#
# ### Q3：面形心积分前向误差界（加到门 A 的面形心容差）
#
# 实现：每个面按 f.p[0] 扇形三角化；面积 = norm(cross(b−a, d−a))/2；16 点 6 阶三角规则（`triangle_rule(6)`）；sq_w = fl(area·q.w)·2；点 = Σ l_k v_k → ξ = (p − lo)/size；dump 用 long double 累加 Σw、Σw·(lo + ξ·size)（按 u 计）。分项：
# - 权：轴向面上叉积只有一个非零分量，|fl(c) − c| ≤ γ_4(|P1| + |P2|)，再经 √ 与 /2 共 γ_6；fl(area·q.w) 再 (1+δ)。Σ_T (|P1|+|P2|)/2 ≤ n_T·D_s·D_t（n_T = 面顶点数 − 2，D_s、D_t 为面在两面内坐标上的范围，含盒误差）。形心偏移 ≤ D_d·Σ|ε_w|/Σŵ。
# - 点：|x̂ − x| ≤ γ_4·S_d + γ_3·h_d（S_d 为单元该轴坐标绝对值上界）。
# - 规则线性缺陷：`cut_exact_oracle.py` 内放 `jacobi`/`gauss01`/`triangle_rule` 的逐位 IEEE double 复刻（同一运算顺序；box x86-64 基线无 FMA），用 Fraction 精确算 δ1 = Σ_k|β_k − 1/3|、δ2 = |Σβ_k − 1|（β_k = Σ_q w_q l_kq / Σ_q w_q），项为 δ1·D_d + δ2·S_d；自检断言 δ1、δ2 ≤ 1e-15（实测 1.04e-16、3.1e-17）。复刻只用来给出规则缺陷 δ1、δ2 的数值（oracle 侧），不要求编译后的 C++ 在各平台逐位复现它；macOS/Apple clang 若因 FMA 收缩使规则节点/权差几个 ulp，δ1、δ2 的变化在 1e-15 量级以下，远小于 1e-13·h 门值，测试在 Linux 与 macOS 上同样有效。
# - 求和：(γ_n + γ_{n−1})·S_d/(1 − γ_{n−1})，n = 该面 n_rec。
# - 写成 c′·u·h/开口占比 的形式：权项 ≤ (6n_T + 1)·u·h·(D_d/h)(D_sD_t/(h_sh_t))/frac，即 **c′ = 6n_T + 1（面 ≤ 6 顶点时 ≤ 25）**；其余项不随占比放大：(4 + 2n_rec)·u·S_d + 3u·h_d + 规则项。PR 列出每组 Q3/h 的最大值。
# - 诊断实测：Q3 最大 2.23e-12·h；G_nosnap_96x32 gid 40–43 face0 z：Q3 = 0.93–3.2e-14·h，2.89e-10·h 由门 A 侧面梯形区间覆盖（距离 0）。
#
# ### 门 B（06:33 裁定 C，不变）
#
# 相对精确真几何：开口/体积占比 ≥ 1e-3 的单元 `|c − c*|` ≤ 1e-13·h_axis；占比 < 1e-3 的薄片 `|c − c*|·占比` ≤ 1e-15·h_axis；面形心用该面开口占比，同理。诊断实测：0 失败，≥1e-3 最大 1.73e-14·h，薄片「误差×占比」最大 4.63e-17·h。
#

P2 = tuple[F, F]
P3 = tuple[F, F, F]
Interval = tuple[F, F]
U = F(1, 2**53)
C_U = 47


@lru_cache(maxsize=256)
def _gamma(n):
    assert type(n) is int and n >= 0 and n*U < 1
    return n*U/(1-n*U)


@lru_cache(maxsize=8192)
def _sqrt_bounds(x):
    """Rational enclosing sqrt, with at least 79 relative binary digits."""
    assert isinstance(x, F) and x >= 0
    if not x:
        return F(0), F(0)
    bits = 80+max(0, (x.denominator.bit_length()-x.numerator.bit_length()+1)//2)
    scale = 1 << bits
    n = isqrt((x.numerator << (2*bits))//x.denominator)
    lo = F(n, scale)
    return lo, lo if lo*lo == x else F(n+1, scale)


def _canonical_ring(ring):
    r = list(_ring_key(ring))
    _planes(tuple(r))  # Convexity and nondegeneracy validation.
    if r[0] == r[-1]:
        r.pop()
    if _area(r) < 0:
        r.reverse()
    start = min(range(len(r)), key=lambda i: (r[i][1], r[i][0]))
    return r[start:]+r[:start]


def _vertex_error(vertex, d):
    return C_U*U*vertex[1][d]


def _cross_bound(p, q, plane, anchor, t, index):
    """U, cases L/S; all scales and lower denominators remain rational."""
    a, b, _ = plane
    point = tuple(p[0][d]+t*(q[0][d]-p[0][d]) for d in range(2))
    ep = tuple(_vertex_error(p, d) for d in range(2))
    eq = tuple(_vertex_error(q, d) for d in range(2))
    magnitude = [max(abs(p[0][d])+ep[d], abs(q[0][d])+eq[d]) for d in range(2)]
    sphi = sum((abs(n)*(magnitude[d]+abs(anchor[d]))
                for d, n in enumerate((a, b))), F(0))
    eps = _gamma(23)*sphi
    shared = [d for d in range(2) if p[0][d] == q[0][d] and p[2][d] and q[2][d]]
    if shared:
        j, k = shared[0], 1-shared[0]
        delta = abs(q[0][k]-p[0][k])
        dlo, dhi = delta-ep[k]-eq[k], delta+ep[k]+eq[k]
        nk = abs((a, b)[k])
        scale = [F(0), F(0)]
        segment = (dhi+ep[k]+eq[k])/(C_U*U)+abs(point[k])
        if nk > 0 and dlo > 0 and 2*eps < nk*dlo:
            rho = 2*eps/(nk*dlo)
            scale[k] = min(segment, sphi*dhi/(nk*dlo*(1-rho))+dhi+abs(point[k]))
        else:
            scale[k] = segment
        fixed = (j == 0, j == 1)
        return point, tuple(scale), fixed, ('L', index)
    # L1 upper bounds enclose the endpoint 2-norm errors and scales.
    eab = max(sum(ep), sum(eq))
    sab = max(sum(p[1]), sum(q[1]))
    delta = tuple(q[0][d]-p[0][d] for d in range(2))
    length_lo, length_hi = _sqrt_bounds(sum((v*v for v in delta), F(0)))
    llo, lhi = length_lo-2*eab, length_hi+2*eab
    _, norm_hi = _sqrt_bounds(a*a+b*b)
    sine = (abs(a*delta[0]+b*delta[1])/norm_hi-2*eab)/lhi
    scale = []
    for d in range(2):
        dhi = abs(delta[d])+ep[d]+eq[d]
        segment = (dhi+max(ep[d], eq[d]))/(C_U*U)+abs(point[d])
        if sine > 0 and llo > 0 and 2*eps < norm_hi*sine*llo:
            rho = 2*eps/(norm_hi*sine*llo)
            value = (sab*(1+1/sine)+sphi*dhi/(norm_hi*sine*llo*(1-rho))
                     +dhi+abs(point[d]))
            scale.append(min(value, segment))
        else:
            scale.append(segment)
    return point, tuple(scale), (False, False), ('S', index)


def _merge_vertices(p, q):
    assert p[0] == q[0]
    order = {'corner': 0, 'L': 1, 'S': 2}
    return (p[0], tuple(max(p[1][d], q[1][d]) for d in range(2)),
            tuple(p[2][d] and q[2][d] for d in range(2)),
            min((p[3], q[3]), key=lambda t: (order[t[0]], t[1])))


def _bounded_step(poly, plane, anchor, snap, index):
    if not poly:
        return []
    a, b, c = plane
    values = [snap_phi(a*v[0][0]+b*v[0][1]-c, a*a+b*b, snap) for v in poly]
    if not any(f > 0 for f in values):
        return poly
    if not any(f < 0 for f in values):
        return []
    out = []

    def append(v):
        if out and out[-1][0] == v[0]:
            out[-1] = _merge_vertices(out[-1], v)
        else:
            out.append(v)

    for i, q in enumerate(poly):
        p, fp, fq = poly[i-1], values[i-1], values[i]
        if (fp > 0 and fq <= 0) or (fp <= 0 and fq > 0):
            t = fp/(fp-fq)
            append(p if t == 0 else q if t == 1 else
                   _cross_bound(p, q, plane, anchor, t, index))
        if fq <= 0:
            append(q)
    if len(out) > 1 and out[0][0] == out[-1][0]:
        out[0] = _merge_vertices(out[0], out.pop())
    return out


@lru_cache(maxsize=32768)
def _clip_bounds_cached(x0, x1, y0, y1, planes, ring, snap):
    poly = [(p, (F(0), F(0)), (True, True), ('corner', -1))
            for p, _ in _rectangle(x0, x1, y0, y1)]
    for index, plane in enumerate(planes):
        poly = _bounded_step(poly, plane, ring[index], snap, index)
    if poly and (len(poly) < 3 or _area([v[0] for v in poly]) == 0):
        poly = []
    if poly:
        start = min(range(len(poly)), key=lambda i: (poly[i][0][1], poly[i][0][0]))
        poly = poly[start:]+poly[:start]
    return tuple(poly)


def clip_bounds(x0, x1, y0, y1, planes, ring, snap):
    assert all(isinstance(v, F) for v in (x0, x1, y0, y1))
    assert x0 < x1 and y0 < y1 and isinstance(snap, F) and snap >= 0
    assert isinstance(ring, list) and all(isinstance(p, tuple) and len(p) == 2
                                         and all(isinstance(v, F) for v in p) for p in ring)
    try:
        canonical = _canonical_ring(ring)
    except ValueError as error:
        raise AssertionError('canonical convex ring required') from error
    assert ring == canonical, 'canonical ring required'
    assert isinstance(planes, list) and all(isinstance(p, tuple) and len(p) == 3
                          and all(isinstance(v, F) for v in p) for p in planes)
    assert tuple(planes) == _planes(tuple(ring)), 'planes differ'
    return list(_clip_bounds_cached(x0, x1, y0, y1, tuple(planes), tuple(ring), snap))


class _Interval:
    __slots__ = ('lo', 'hi')

    def __init__(self, lo, hi=None):
        self.lo, self.hi = lo, lo if hi is None else hi

    def __add__(self, other):
        return _Interval(self.lo+other.lo, self.hi+other.hi)

    def __sub__(self, other):
        return _Interval(self.lo-other.hi, self.hi-other.lo)

    def __mul__(self, other):
        values = (self.lo*other.lo, self.lo*other.hi, self.hi*other.lo, self.hi*other.hi)
        return _Interval(min(values), max(values))

    def inv(self):
        if self.lo <= 0 <= self.hi:
            raise ZeroDivisionError('interval area includes zero')
        return _Interval(1/self.hi, 1/self.lo)


class _Derivative:
    __slots__ = ('value', 'gradient')

    def __init__(self, value, gradient):
        self.value, self.gradient = value, gradient

    def __add__(self, other):
        return _Derivative(self.value+other.value,
                           [x+y for x, y in zip(self.gradient, other.gradient)])

    def __sub__(self, other):
        return _Derivative(self.value-other.value,
                           [x-y for x, y in zip(self.gradient, other.gradient)])

    def __mul__(self, other):
        return _Derivative(self.value*other.value,
                           [x*other.value+self.value*y for x, y in zip(self.gradient, other.gradient)])

    def __truediv__(self, other):
        inv = other.value.inv()
        ratio = self.value*inv
        return _Derivative(ratio, [(x-ratio*y)*inv
                                   for x, y in zip(self.gradient, other.gradient)])


def _centroid_formula(pts, axes, comp):
    if comp not in axes:
        return pts[0][comp]
    cross, moment = None, None
    s, t = axes
    for i, p in enumerate(pts):
        q = pts[i-1]
        c = q[s]*p[t]-p[s]*q[t]
        term = (q[comp]+p[comp])*c
        cross = c if cross is None else cross+c
        moment = term if moment is None else moment+term
    return moment/(cross+cross+cross)


def _replace_points(pts, idxs, values):
    result = [list(p) for p in pts]
    for (i, d), value in zip(idxs, values):
        result[i][d] = value
    return result


def _distance(actual, interval):
    return max(interval[0]-actual, actual-interval[1], F(0))


@lru_cache(maxsize=65536)
def _corner_range(pts, idxs, boxes, axes, comp):
    values = []
    for choice in product(*[(lo,) if lo == hi else (lo, hi) for lo, hi in boxes]):
        try:
            values.append(_centroid_formula(_replace_points(pts, idxs, choice), axes, comp))
        except ZeroDivisionError:
            continue
    return (min(values), max(values)) if values else None


def _mean_value_range(pts, idxs, boxes, axes, comp):
    n = len(idxs)
    mid = [(lo+hi)/2 for lo, hi in boxes]
    center = _centroid_formula(_replace_points(pts, idxs, mid), axes, comp)
    zero = [_Interval(F(0)) for _ in range(n)]
    points = [[_Derivative(_Interval(v), zero) for v in p] for p in pts]
    for j, ((i, d), (lo, hi)) in enumerate(zip(idxs, boxes)):
        gradient = [_Interval(F(int(k == j))) for k in range(n)]
        points[i][d] = _Derivative(_Interval(lo, hi), gradient)
    result = _centroid_formula(points, axes, comp)
    radius = sum((max(abs(g.lo), abs(g.hi))*(hi-lo)/2
                  for g, (lo, hi) in zip(result.gradient, boxes)), F(0))
    return center-radius, center+radius


def _centroid_problem(fun, pts, idxs, boxes, comp, actual, tol):
    assert fun in ('polygon_xy', 'side_sz', 'constant') and type(comp) is int and 0 <= comp < 3
    assert isinstance(actual, F) and isinstance(tol, F) and tol >= 0
    assert isinstance(pts, list) and pts and all(isinstance(p, tuple) and len(p) == 3
                                    and all(isinstance(v, F) for v in p) for p in pts)
    assert isinstance(idxs, list) and isinstance(boxes, list) and len(idxs) == len(boxes)
    assert all(isinstance(ix, tuple) and len(ix) == 2 and all(type(i) is int for i in ix)
               and 0 <= ix[0] < len(pts) and 0 <= ix[1] < 3 for ix in idxs)
    assert len(set(idxs)) == len(idxs)
    assert all(isinstance(b, tuple) and len(b) == 2 and all(isinstance(v, F) for v in b)
               and b[0] <= pts[i][d] <= b[1] for (i, d), b in zip(idxs, boxes))
    if fun == 'constant':
        assert len(pts) == 1 and not idxs
        return ()
    assert len(pts) >= 3
    if fun == 'side_sz':
        assert len(pts) == 4, 'side centroid requires four vertices'
    fixed = 2 if fun == 'polygon_xy' else next(
        (d for d in (0, 1) if len({p[d] for p in pts}) == 1), None)
    assert fixed is not None and len({p[fixed] for p in pts}) == 1
    assert all(d != fixed for _, d in idxs)
    axes = tuple(d for d in range(3) if d != fixed)
    projected = [(p[axes[0]], p[axes[1]]) for p in pts]
    assert _area(projected) != 0, 'zero area centroid'
    return axes


def _centroid_result(fun, pts, idxs, boxes, comp, actual, tol):
    axes = _centroid_problem(fun, pts, idxs, boxes, comp, actual, tol)
    if not axes or comp not in axes:
        distance = abs(actual-pts[0][comp])
        return ('pass' if distance <= tol else 'fail'), distance, 0
    pts, idxs, boxes = tuple(pts), tuple(idxs), tuple(boxes)
    inner = _corner_range(pts, idxs, boxes, axes, comp)
    distance = _distance(actual, inner) if inner else None
    if all(lo == hi for lo, hi in boxes):
        assert distance is not None
        return ('pass' if distance <= tol else 'fail'), distance, 0

    def visit(current, depth):
        inner = _corner_range(pts, idxs, current, axes, comp)
        if inner and _distance(actual, inner) <= tol:
            return 'pass', 0
        try:
            outer = _mean_value_range(pts, idxs, current, axes, comp)
            if _distance(actual, outer) > tol:
                return 'fail', 0
        except ZeroDivisionError:
            pass  # Never decide using an undefined interval quotient.
        if depth == 10 or not current or all(lo == hi for lo, hi in current):
            return 'fail', 0
        index = max(range(len(current)), key=lambda i: current[i][1]-current[i][0])
        lo, hi = current[index]
        mid, count = (lo+hi)/2, 0
        for half in ((lo, mid), (mid, hi)):
            outcome, children = visit(current[:index]+(half,)+current[index+1:], depth+1)
            count += 1+children
            if outcome == 'pass':
                return 'pass', count
        return 'fail', count

    result, splits = visit(boxes, 0)
    return result, distance, splits


def centroid_check(fun, pts, idxs, boxes, comp, actual, tol):
    return _centroid_result(fun, pts, idxs, boxes, comp, actual, tol)[0]


def _jacobi(n, alpha, x):
    p, dp, q, dq = 1.0, 0.0, 0.0, 0.0
    for k in range(1, n+1):
        if k == 1:
            t = (alpha+(alpha+2)*x)/2
            dt = float(alpha+2)/2
        else:
            h = float(2*k+alpha)
            a = (h-1)*(h*(h-2)*x+alpha*alpha)
            da = (h-1)*h*(h-2)
            b = 2*(k+alpha-1)*(k-1)*h
            c = 2*k*(k+alpha)*(h-2)
            t = (a*p-b*q)/c
            dt = (da*p+a*dp-b*dq)/c
        q, dq, p, dp = p, dp, t, dt
    return p, dp


def _gauss01(n, alpha=0):
    bounds = [-1.0]
    if n > 1:
        bounds.extend(2*u-1 for u, _ in _gauss01(n-1, alpha))
    bounds.append(1.0)
    out = []
    for i in range(n):
        lo, hi = bounds[i:i+2]
        x = (lo+hi)/2
        plo = _jacobi(n, alpha, lo)[0]
        for _ in range(100):
            p, dp = _jacobi(n, alpha, x)
            dx = p/dp
            if abs(dx) <= 4*2.0**-52*max(1.0, abs(x)):
                x -= dx
                break
            if (p > 0) == (plo > 0):
                lo, plo = x, p
            else:
                hi = x
            candidate = x-dx
            x = candidate if lo < candidate < hi else (lo+hi)/2
        else:
            raise AssertionError('Gauss-Jacobi replica did not converge')
        _, dp = _jacobi(n, alpha, x)
        out.append(((x+1)/2, 1/((1-x*x)*dp*dp)))
    return out


def triangle_rule_replica(degree):
    """IEEE binary64 operation order, x86-64 baseline without FMA.

    Adapted from the audit's rule_replica.py; only linear rule defects enter
    truth. No quadrature coordinates or weights are used as exact integrals.
    """
    assert type(degree) is int and degree >= 0
    n = degree//2+1
    return [(((1-u)*(1-v), u, (1-u)*v), wu*wv)
            for u, wu in _gauss01(n, 1) for v, wv in _gauss01(n)]


@lru_cache(maxsize=16)
def rule_defects(degree=6):
    rule = triangle_rule_replica(degree)
    total = sum((F(w) for _, w in rule), F(0))
    beta = [sum((F(w)*F(l[d]) for l, w in rule), F(0))/total for d in range(3)]
    return dict(d1=sum((abs(v-F(1, 3)) for v in beta), F(0)),
                d2=abs(sum(beta)-1), W2=2*total)


@lru_cache(maxsize=32768)
def _face_geometry(vertices, errors, side):
    normal = side//2
    s, t = [d for d in range(3) if d != normal]
    assert len({p[normal] for p in vertices}) == 1 and all(e[normal] == 0 for e in errors)
    projected = [(p[s], p[t]) for p in vertices]
    area = abs(_area(projected))
    # L1 perimeter is a rational upper bound on Euclidean perimeter.
    perimeter = sum((abs(p[s]-vertices[i-1][s])+abs(p[t]-vertices[i-1][t])
                     for i, p in enumerate(vertices)), F(0))
    halfwidth = max(v for e in errors for v in e)
    alo = area-2*perimeter*halfwidth
    assert alo > 0, 'Q3 nonpositive prior area lower bound'
    dimensions = tuple(max(p[d]+e[d] for p, e in zip(vertices, errors))
                       -min(p[d]-e[d] for p, e in zip(vertices, errors)) for d in range(3))
    w = (_gamma(6)+U*(1+_gamma(6)))*(len(vertices)-2)*dimensions[s]*dimensions[t]
    assert alo > w, 'Q3 nonpositive prior weight lower bound'
    return dimensions, alo, w


def face_integration_bound(face_pts, e_max, axis, h, s_max, n_rec):
    assert isinstance(face_pts, dict) and set(face_pts) == {'vertices'}
    vertices = face_pts['vertices']
    assert isinstance(vertices, list) and len(vertices) >= 3
    assert isinstance(e_max, list) and len(e_max) == len(vertices)
    for p, e in zip(vertices, e_max):
        assert isinstance(p, tuple) and len(p) == 3 and all(isinstance(v, F) for v in p)
        assert isinstance(e, tuple) and len(e) == 3 and all(isinstance(v, F) and v >= 0 for v in e)
    assert isinstance(axis, tuple) and len(axis) == 2 and all(type(v) is int for v in axis)
    side, comp = axis
    assert 0 <= side < 6 and 0 <= comp < 3
    assert isinstance(h, tuple) and len(h) == 3 and all(isinstance(v, F) and v > 0 for v in h)
    assert isinstance(s_max, tuple) and len(s_max) == 3 and all(isinstance(v, F) and v >= 0 for v in s_max)
    assert type(n_rec) is int and n_rec > 0
    assert _gamma(n_rec-1) < 1, 'Q3 nonpositive summation denominator'
    dims, alo, w = _face_geometry(tuple(vertices), tuple(e_max), side)
    rule = rule_defects(6)
    w2, d1, d2 = rule['W2'], rule['d1'], rule['d2']
    return (dims[comp]*w2*w/(w2*alo-w2*w)+_gamma(4)*s_max[comp]+_gamma(3)*h[comp]
            +d1*dims[comp]+d2*s_max[comp]
            +(_gamma(n_rec)+_gamma(n_rec-1))*s_max[comp]/(1-_gamma(n_rec-1)))


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
            # Snapped caps can leave the exact plane. Closure uses the actual
            # oriented segment, while normal checks still use the plane normal.
            eb_vectors.append(((q[1]-p[1])*hz, -(q[0]-p[0])*hz, F(0)))
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
    canonical = _canonical_ring(ring)

    def margin(vertex, index):
        a, b, c = planes[index]
        p = vertex[0]
        error = tuple(_vertex_error(vertex, d) for d in range(2))
        sphi = sum((abs(n)*(abs(p[d])+error[d]+abs(canonical[index][d]))
                    for d, n in enumerate((a, b))), F(0))
        epsilon = _gamma(23)*sphi+abs(a)*error[0]+abs(b)*error[1]
        f = abs(a*p[0]+b*p[1]-c)
        nlo, nhi = _sqrt_bounds(a*a+b*b)
        # Bounds on | |f| - snap*|N| | avoid subtracting approximate roots.
        gap = snap*nlo-f if f*f <= snap*snap*(a*a+b*b) else f-snap*nhi
        assert gap > epsilon, 'uncertain snap decision'

    for i in range(len(ax[0])-1):
        for j in range(len(ax[1])-1):
            original = [(ax[0][i], ax[1][j]), (ax[0][i+1], ax[1][j]),
                        (ax[0][i+1], ax[1][j+1]), (ax[0][i], ax[1][j+1])]
            mixed = False
            evaluated = []
            for index, (a, b, c) in enumerate(planes):
                evaluated.append(index)
                values = [a*p[0]+b*p[1]-c for p in original]
                if any(ambiguous(f, a*a+b*b, snap) for f in values):
                    raise AssertionError('ambiguous snap case')
                values = [snap_phi(f, a*a+b*b, snap) for f in values]
                if any(f > 0 for f in values):
                    mixed = any(f < 0 for f in values)
                    break
            if mixed:
                poly = original
                for a, b, c in planes:
                    # Only vertices actually evaluated at this clipping stage.
                    for p in poly:
                        if ambiguous(a*p[0]+b*p[1]-c, a*a+b*b, snap):
                            raise AssertionError('ambiguous snap case')
                    poly = clip_halfplane(poly, a, b, c, snap=snap)
            bounded = [(p, (F(0), F(0)), (True, True), ('corner', -1)) for p in original]
            for index in evaluated:
                for v in bounded:
                    margin(v, index)
            if mixed:
                for index, plane in enumerate(planes):
                    for v in bounded:
                        margin(v, index)
                    bounded = _bounded_step(bounded, plane, canonical[index], snap, index)
