#!/usr/bin/env python3
"""PW 系列（分段移位跨 Tg）的半解析参考解：两端固定的单轴杆（单元级 SC-PW）。
本构：1D 广义 Maxwell（Prony），时温等效 ξ = ∫ dt / a_T(T)。机械应变 ε_m = -ε_th(T)（杆长被固定）。
对每个增量用"约化时间内应变率恒定"的精确指数积分；逐级加密直到收敛（报告收敛差）。
注：同一组 Prony 用于剪切与体积时，三维单元（一个方向固定、侧面自由）的 σ_xx 与本 1D 解一致。"""
import math, json, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from derive_cards import prony_scaled

E0, Einf = 7.9e3, 0.2e3            # MPa，UF_C4（Kim2016 两点值）
TERMS, _, _ = prony_scaled(7.9, 0.2)
Ei = [a*E0 for a, _ in TERMS]; tau = [t for _, t in TERMS]
ag, ar, Tg = 26e-6, 107e-6, 122.0
Tsf, Tend, rate = 150.0, 25.0, 10.0/60.0   # K/s
k1 = 2.45104e7/1.87386e8                   # 单一 WLF 的等效斜率（dec/K）
def log_aT_single(T): return -2.45104e7*(T-Tg)/(-1.87386e8+(T-Tg)) * (-1)  # 文献式 log aT = -C1 ΔT/(C2+ΔT)
def log_aT_pw(T, k2=0.05):
    return -k1*(T-Tg) if T >= Tg else -k2*(T-Tg)
def eth(T):
    F = lambda x: ag*(x-Tg) if x <= Tg else ar*(x-Tg)
    return F(T)-F(Tsf)
def T_of_t(t, t1):
    return Tsf - rate*t if t <= t1 else Tend

def run(log_aT, n_cool, n_hold, hold=3600.0, sub=20):
    t1 = (Tsf-Tend)/rate
    h = [0.0]*len(Ei); eps_prev = 0.0; out = {}
    times = [t1*i/n_cool for i in range(n_cool+1)] + [t1 + hold*(i/n_hold)**2 for i in range(1, n_hold+1)]
    for a, b in zip(times[:-1], times[1:]):
        # Δξ：子区间中点求积
        dxi = sum((b-a)/sub / 10**log_aT(T_of_t(a+(j+0.5)*(b-a)/sub, t1)) for j in range(sub))
        eps = -eth(T_of_t(b, t1)); de = eps-eps_prev; eps_prev = eps
        for i in range(len(Ei)):
            x = dxi/tau[i]
            if x > 700: e = 0.0; g = 1.0/x
            elif x < 1e-12: e = 1.0; g = 1.0
            else: e = math.exp(-x); g = (1-e)/x
            h[i] = e*h[i] + Ei[i]*g*de
        if abs(b-t1) < 1e-9: out["sigma_end_cool_MPa"] = Einf*eps + sum(h)
    out["sigma_end_hold_MPa"] = Einf*eps_prev + sum(h)
    return out

if __name__ == "__main__":
    res = {}
    for name, f in [("single_WLF", log_aT_single), ("piecewise_k2_0.05", log_aT_pw)]:
        coarse = run(f, 1250, 400); fine = run(f, 5000, 1600)
        res[name] = dict(fine=fine, rel_change_coarse_to_fine={k: abs(fine[k]-coarse[k])/abs(fine[k]) for k in fine})
    print(json.dumps(res, indent=1))
