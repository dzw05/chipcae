#!/usr/bin/env python3
"""chipcae 公开翘曲基准：共享 QoI 后处理参考实现 (v0.1)

各求解器都只导出"表面节点坐标 + 位移"，然后由本脚本计算 QoI，
避免各自的后处理口径不同。输入 CSV 列：x,y,z,ux,uy,uz（单位 mm；变形前坐标）。
四分之一模型请先用 mirror_quarter() 镜像为全模型再计算。
"""
import numpy as np

def mirror_quarter(P):
    """P: (n,6) 数组 x,y,z,ux,uy,uz，x>=0,y>=0。按 x=0、y=0 两个对称面镜像（去重）。"""
    out = [P]
    Q = P.copy(); Q[:, 0] *= -1; Q[:, 3] *= -1; out.append(Q)
    Q = P.copy(); Q[:, 1] *= -1; Q[:, 4] *= -1; out.append(Q)
    Q = P.copy(); Q[:, 0] *= -1; Q[:, 1] *= -1; Q[:, 3] *= -1; Q[:, 4] *= -1; out.append(Q)
    A = np.vstack(out)
    _, idx = np.unique(np.round(A[:, :3], 9), axis=0, return_index=True)
    return A[np.sort(idx)]

def interp_grid(P, xs, ys):
    """在规则网格上插值 uz（表面节点含中节点，线性三角插值）。需要 scipy；无 scipy 时退回最近节点。"""
    X, Y = np.meshgrid(xs, ys, indexing="ij")
    try:
        from scipy.interpolate import LinearNDInterpolator
        f = LinearNDInterpolator(P[:, :2], P[:, 5])
        W = f(X, Y)
    except ImportError:
        d = (X[..., None]-P[None, None, :, 0])**2 + (Y[..., None]-P[None, None, :, 1])**2
        W = P[np.argmin(d, axis=-1), 5]
    return X, Y, W

def best_fit_plane(X, Y, W):
    m = np.isfinite(W)
    A = np.c_[np.ones(m.sum()), X[m], Y[m]]
    coef, *_ = np.linalg.lstsq(A, W[m], rcond=None)
    R = W - (coef[0] + coef[1]*X + coef[2]*Y)
    return coef, R

def qoi_surface(X, Y, W):
    """返回 W_bfp（相对最小二乘平面的峰谷值, >=0）与 S_cc（中心减四角平均，带号；z 向上，正 = 中心高 = ∩）。"""
    coef, R = best_fit_plane(X, Y, W)
    Wbfp = np.nanmax(R) - np.nanmin(R)
    i0 = np.unravel_index(np.nanargmin(X**2 + Y**2), X.shape)
    corners = [(0, 0), (0, -1), (-1, 0), (-1, -1)]
    Scc = R[i0] - np.mean([R[c] for c in corners])
    return dict(W_bfp=float(Wbfp), S_cc=float(Scc), plane=coef.tolist())

def curvature_along(s, w, smax):
    """在 |s|<=smax 上拟合 w = a s^2 + b s + c，返回 kappa = -2a（与 Hsueh 号约定一致：正 = ∩）。"""
    m = np.abs(s) <= smax + 1e-12
    a, b, c = np.polyfit(s[m], w[m], 2)
    return float(-2*a)

if __name__ == "__main__":
    # 自检：球面 w = -k r^2/2 + 刚体倾斜，应得 W_bfp = S_cc = k*L^2/4，kappa = k
    k, L = 1.2e-3, 40.0
    xs = ys = np.linspace(-L/2, L/2, 81)
    X, Y = np.meshgrid(xs, ys, indexing="ij")
    W = -k*(X**2 + Y**2)/2 + 1e-3*X - 2e-3*Y + 0.05
    q = qoi_surface(X, Y, W)
    s = xs*np.sqrt(2); wd = np.array([W[i, i] for i in range(len(xs))])
    print(q["W_bfp"], q["S_cc"], k*L**2/4, curvature_along(s - 0, wd + 1e-3*0, 20.0))
