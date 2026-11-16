#!/usr/bin/env python3
"""chipcae 公开翘曲基准：派生量计算脚本 (v0.1)

只做确定性计算，不引入新的物理数据：
  1) 焊点/底填均质层的面内等效性质（Park 公式，转引自 Kim et al. 2016 式(1)-(4)）
  2) B0/B1 全覆盖退化叠层的 Hsueh(2002) 曲率参考值（SC-1），并与宽板 Timoshenko 交叉核对（SC-2）
  3) 聚合物 E(T) 表（chipcae 选定的 Tg±20°C 对数线性过渡）与相对 T_sf 的割线 CTE 表
  4) 由 Chen et al. 2022 EMC Prony 形状缩放得到的 chipcae 选定底填 Prony
  5) 各网格级别的单元数估计（mesh_counts.py）
  6) P-KIM2016 均质层：Kim 2016 Table 3 组分重推（kim2016_homogenized；SnAg E(T) tbd）
  Prony 形状缩放系数 s 的灵敏度卡见 prony_scale.py
所有输入值在 YAML 里都带 source/status 标签；本脚本输出的数值属于"派生(derived)"。
"""
import json, math, sys

# ---------------------------------------------------------------- 1) Park 均质化
def park_inplane(Es, nus, als, cs, Eu, nuu, alu):
    """Kim et al. 2016 (Microelectron. Reliab. 65:234) eqs (1)-(4), 引 Park et al.
    s = 纤维(焊点/铜柱), u = 基体(底填). 返回 (E_xy, alpha_xy, alpha_z, nu_xz)."""
    cu = 1.0 - cs
    inv = cs/Es + cu/Eu - cs*cu*(nus*Eu - nuu*Es)**2/(Es*Eu*(cs*Es + cu*Eu))
    Exy = 1.0/inv
    az = (Es*als*cs + Eu*alu*cu)/(cs*Es + cu*Eu)
    nuxz = cs*nus + cu*nuu
    axy = (1+nuu)*alu*cu + (1+nus)*als*cs - az*nuxz
    return Exy, axy, az, nuxz

# ---------------------------------------------------------------- 2) Hsueh 多层曲率
def hsueh(sub, layers, dT=None, eth=None):
    """sub=(Ebar,t,alpha), layers=[(Ebar,t,alpha),...] 自底向上（层1贴衬底）。
    eth: 可选，各层热应变列表 [eps_s, eps_1, ...] 代替 alpha*dT。
    返回 (c, b, kappa)；kappa>0 表示 z 向上时上表面伸长 -> 中心高于边缘(∩)。"""
    Es, ts, als = sub
    if eth is None:
        eth = [als*dT] + [L[2]*dT for L in layers]
    es = eth[0]; el = eth[1:]
    num_c = Es*ts*es + sum(L[0]*L[1]*e for L, e in zip(layers, el))
    den = Es*ts + sum(L[0]*L[1] for L in layers)
    c = num_c/den
    h = [0.0]
    for L in layers: h.append(h[-1] + L[1])
    # 注意：warpage_benchmarks.md §3.3 草案中 b 的层项符号有误；按定义 b=∫Ēz dz/∫Ē dz 修正为下式
    b = (-Es*ts**2 + sum(L[0]*L[1]*(2*h[i]+L[1]) for i, L in enumerate(layers)))/(2*den)
    num = 3*(Es*(c-es)*ts**2 - sum(L[0]*L[1]*(c-e)*(2*h[i]+L[1]) for i, (L, e) in enumerate(zip(layers, el))))
    dd = Es*ts**2*(2*ts+3*b) + sum(L[0]*L[1]*(6*h[i]**2 + 6*h[i]*L[1] + 2*L[1]**2 - 3*b*(2*h[i]+L[1])) for i, L in enumerate(layers))
    return c, b, num/dd

def timoshenko(E1, t1, a1, E2, t2, a2, dT):
    """Timoshenko 1925 通式（见 warpage_benchmarks.md §2.1），层1在下、层2在上。"""
    m = t1/t2; n = E1/E2; h = t1+t2
    return 6*(a2-a1)*dT*(1+m)**2/(h*(3*(1+m)**2 + (1+m*n)*(m**2 + 1/(m*n))))

# ---------------------------------------------------------------- 3) 表格工具
def poly_E_table(Eg, Er, Tg, Tlo=25.0, Thi=260.0, half=20.0, step=5.0):
    """chipcae 选定：T<=Tg-half 取玻璃态 Eg；T>=Tg+half 取橡胶态 Er；中间 log10(E) 线性；
    表点之间按 E 线性插值（表点足够密，插值差异可忽略）。"""
    pts = [Tlo]
    t = Tg - half
    while t <= Tg + half + 1e-9:
        if Tlo < t < Thi: pts.append(round(t, 3))
        t += step
    pts.append(Thi)
    out = []
    for T in pts:
        if T <= Tg-half: E = Eg
        elif T >= Tg+half: E = Er
        else:
            f = (T-(Tg-half))/(2*half)
            E = 10**(math.log10(Eg) + f*(math.log10(Er)-math.log10(Eg)))
        out.append([T, round(E, 6)])
    return out

def eth_bilinear(T, Tsf, ag, ar, Tg):
    """瞬时 CTE 在 Tg 处阶跃（ag 以下, ar 以上）。返回 eps_th(T) = ∫_{Tsf}^{T} α_in dθ。"""
    def F(x):  # 原函数，F(Tg)=0
        return ag*(x-Tg) if x <= Tg else ar*(x-Tg)
    return F(T) - F(Tsf)

def secant_table(Tsf, ag, ar, Tg, temps):
    rows = []
    for T in temps:
        if abs(T-Tsf) < 1e-9:
            a = ag if Tsf < Tg else ar   # 极限值（T_sf 恰在 Tg 时取 ar，YAML 中注明）
        else:
            a = eth_bilinear(T, Tsf, ag, ar, Tg)/(T-Tsf)
        rows.append([T, round(a*1e6, 6)])  # ppm/K
    return rows

# ---------------------------------------------------------------- 4) Prony
CHEN_EI = [0.14485,0.20816,0.23859,0.23046,0.46783,0.42749,1.11779,1.91552,2.49683,2.63466,2.19706,
           1.59056,1.08914,0.71367,0.52184,0.39181,0.35507,0.27711,0.33030,0.21223,0.20900,0.21489,
           0.09099,0.20694,0.55236]   # GPa, Chen et al. 2022 Table 4
CHEN_TAU = [1e10,1e9,1e6,1e5,1e4,1e3,1e2,1e1,1e0,1e-1,1e-2,1e-3,1e-4,1e-5,1e-6,1e-7,1e-8,1e-9,1e-10,
            1e-11,1e-12,1e-13,1e-14,1e-15,1e-16]  # s
CHEN_EINF = 1.1  # GPa

def prony_scaled(E0, Einf):
    E0c = sum(CHEN_EI) + CHEN_EINF
    w = [e/E0c for e in CHEN_EI]
    s = sum(w)
    target = 1.0 - Einf/E0
    return [[round(wi*target/s, 6), t] for wi, t in zip(w, CHEN_TAU)], E0c, s

# ---------------------------------------------------------------- 5) P-KIM2016 组分（Kim 2016 Table 3, p.238）
# P-KIM2016（validation）的均质层只用 Kim 2016 Table 3 组分重新推导；公式仍为上面的 park_inplane（式 1-4）。
# 默认 = Kim/Park 面内等效、各向同性用法（Kim 2016 p.236：in-plane effective property 作 isotropic）。
# 备选（不作默认、不生成冻结卡）：Voigt 横观各向同性卡——式 (1)-(4) 不给 G_xz、ν_xy，备选卡参数 tbd；
# Halpin–Tsai 不作默认，本脚本不实现。
CU_KIM2016 = dict(E=125.0, nu=0.3, a=16.5e-6, cite="Kim2016 Table 3 (p.238) Cu")
# SnAg：Table 3 给 ν = 0.4、CTE = 21.5 ppm/°C；E 为「−74.995 T + 52,582」，转引 Lau 1996（Kim 文献 [14]）。
# 该式的单位与温度标度（°C 还是 K、MPa 还是 kPa）原文未注明，Lau 1996 原文尚未核对 → E = None（tbd），
# 不进冻结卡。核对后把 E(T) 传给 kim2016_homogenized(snag_E_GPa=...) 即可。
SNAG_KIM2016 = dict(E=None, nu=0.4, a=21.5e-6, cite="Kim2016 Table 3 (p.238) SnAg; E(T) 转引 Lau 1996 [14]，tbd")
UF_INTERPOSER_KIM2016 = dict(Eg=7.9, Er=0.2, nu=0.3, ag=26e-6, ar=107e-6, Tg=122.0)   # Kim 2016 Table 3 Underfill (for interposer)
UF_ASIC_MEM_KIM2016 = dict(Eg=7.5, Er=0.1, nu=0.3, ag=30e-6, ar=105e-6, Tg=135.0)     # Kim 2016 Table 3 Underfill (for ASIC and memory)
PHI_C4_KIM2016 = 0.10     # Kim 2016 p.239「about 10% in C4/underfill layer」
PHI_UBUMP_KIM2016 = 0.03  # Kim 2016 p.239「less than 3% in micro bump/underfill layer」，取上限（chosen）


def _hom_pair(fib, uf, cs):
    g = park_inplane(fib["E"], fib["nu"], fib["a"], cs, uf["Eg"], uf["nu"], uf["ag"])
    r = park_inplane(fib["E"], fib["nu"], fib["a"], cs, uf["Er"], uf["nu"], uf["ar"])
    return dict(cs=cs, glassy=dict(E_GPa=round(g[0], 4), alpha_ppm=round(g[1]*1e6, 4), alpha_z_ppm=round(g[2]*1e6, 4), nu=round(g[3], 4)),
                rubbery=dict(E_GPa=round(r[0], 4), alpha_ppm=round(r[1]*1e6, 4), alpha_z_ppm=round(r[2]*1e6, 4), nu=round(r[3], 4)),
                Tg=uf["Tg"])


def kim2016_homogenized(snag_E_GPa=None):
    """P-KIM2016 均质层（Kim 2016 Table 3 组分 + 式 1-4）。
    微凸点层：纤维按 Cu（chosen 简化，沿用 X/PW 卡做法；Kim p.239 称该层为 solder 体积分数），φ = 0.03。
    C4 层：纤维 SnAg，φ = 0.10；SnAg E(T) 未核对时返回 status = tbd，不给数值。"""
    out = {"I25D_UBUMP_UF_KIM2016": dict(_hom_pair(CU_KIM2016, UF_ASIC_MEM_KIM2016, PHI_UBUMP_KIM2016),
                                         fiber="Cu (Kim2016 Table 3)", status="derived")}
    if snag_E_GPa is None:
        out["I25D_C4_UF_KIM2016"] = dict(cs=PHI_C4_KIM2016, status="tbd",
                                         reason="SnAg E(T) 转引 Lau 1996，式中单位/温度标度未注明，原文未核对")
    else:
        fib = dict(SNAG_KIM2016, E=snag_E_GPa)
        out["I25D_C4_UF_KIM2016"] = dict(_hom_pair(fib, UF_INTERPOSER_KIM2016, PHI_C4_KIM2016),
                                         fiber="SnAg (Kim2016 Table 3)", status="provisional_not_frozen")
    return out


# ---------------------------------------------------------------- main
def main():
    out = {}
    # 材料（与 YAML 一致；来源见 YAML）
    Si = dict(E=131.0, nu=0.28, a=2.8e-6)              # Chen 2022 Table 7 (Shih 2023 Table3: 131/0.30/2.8)
    Cu = dict(E=125.0, nu=0.34, a=17e-6)               # Chen 2022 Table 7
    SAC = dict(E=51.0, nu=0.40, a=23.5e-6)             # Depiver et al. 2024 preprint Table 4
    UF_C4 = dict(Eg=7.9, Er=0.2, nu=0.3, ag=26e-6, ar=107e-6, Tg=122.0)   # Kim 2016 Table 3
    UF_MB = dict(Eg=7.5, Er=0.1, nu=0.3, ag=30e-6, ar=105e-6, Tg=135.0)   # Kim 2016 Table 3
    SUB = dict(E25=31.0, nu=0.3, a=16e-6)              # Kim 2016 Table 3 (isotropic homogenized)
    EMC = dict(E0=sum(CHEN_EI)+CHEN_EINF, Einf=1.1, Eel=19.0, nu=0.26, ag=7e-6, ar=22e-6, Tg=143.0)

    hom = {}
    for name, fib, uf, cs in [("FCBGA_BUMP_UF", SAC, UF_C4, 0.10), ("I25D_C4_UF", SAC, UF_C4, 0.10),
                              ("I25D_UBUMP_UF", Cu, UF_MB, 0.03)]:
        g = park_inplane(fib["E"], fib["nu"], fib["a"], cs, uf["Eg"], uf["nu"], uf["ag"])
        r = park_inplane(fib["E"], fib["nu"], fib["a"], cs, uf["Er"], uf["nu"], uf["ar"])
        hom[name] = dict(cs=cs, glassy=dict(E_GPa=round(g[0],4), alpha_ppm=round(g[1]*1e6,4), alpha_z_ppm=round(g[2]*1e6,4), nu=round(g[3],4)),
                         rubbery=dict(E_GPa=round(r[0],4), alpha_ppm=round(r[1]*1e6,4), alpha_z_ppm=round(r[2]*1e6,4), nu=round(r[3],4)),
                         Tg=uf["Tg"])
    out["homogenized"] = hom
    hk = kim2016_homogenized()
    out["homogenized_P_KIM2016"] = hk

    eb = lambda E, nu: E/(1-nu)
    # ---- SC-1 FCBGA 全覆盖：衬底 1.2 + 均质层 0.025 + 芯片 0.78, Tsf=150 -> 25
    Tsf_f, Tend = 150.0, 25.0
    hb = hom["FCBGA_BUMP_UF"]["glassy"]
    sub = (eb(SUB["E25"], SUB["nu"]), 1.2, SUB["a"])
    lay = [(eb(hb["E_GPa"], hb["nu"]), 0.025, hb["alpha_ppm"]*1e-6), (eb(Si["E"], Si["nu"]), 0.78, Si["a"])]
    c, b, k = hsueh(sub, lay, dT=Tend-Tsf_f)
    L = 40.0
    out["SC1_FCBGA_B0"] = dict(kappa_per_mm=k, c=c, b_mm=b, Scc_um_spherical_40mm=k*L**2/4*1e3, dT=Tend-Tsf_f)
    # 二层（无均质层）与 Timoshenko 宽板
    c2, b2, k2 = hsueh(sub, [lay[1]], dT=Tend-Tsf_f)
    kt = timoshenko(sub[0], 1.2, SUB["a"], lay[1][0], 0.78, Si["a"], Tend-Tsf_f)
    out["SC2_FCBGA_bilayer"] = dict(kappa_hsueh=k2, kappa_timoshenko_wide=kt, rel_diff=abs(k2-kt)/abs(kt))
    # B1 at 25°C: 底填均质层热应变跨 Tg（只有均质层是聚合物）; 弹性取 25°C 值
    hr = hom["FCBGA_BUMP_UF"]
    eps_h = eth_bilinear(Tend, Tsf_f, hr["glassy"]["alpha_ppm"]*1e-6, hr["rubbery"]["alpha_ppm"]*1e-6, hr["Tg"])
    eth = [SUB["a"]*(Tend-Tsf_f), eps_h, Si["a"]*(Tend-Tsf_f)]
    c1, b1, k1 = hsueh(sub, lay, eth=eth)
    out["SC1_FCBGA_B1_25C"] = dict(kappa_per_mm=k1, Scc_um_spherical_40mm=k1*L**2/4*1e3)
    # thin-core variant: 衬底 0.4 mm
    sub_t = (sub[0], 0.4, sub[2])
    ct, bt, kth = hsueh(sub_t, lay, dT=Tend-Tsf_f)
    out["SC1_FCBGA_B0_thin0p4"] = dict(kappa_per_mm=kth, Scc_um_spherical_40mm=kth*L**2/4*1e3)

    # ---- SC-1 2.5D 全覆盖：衬底1.15 + C4层0.10 + 中介层0.10 + 微凸点层0.025 + SoC 0.40, Tsf=165
    Tsf_i = 165.0
    hc = hom["I25D_C4_UF"]["glassy"]; hm = hom["I25D_UBUMP_UF"]["glassy"]
    sub2 = (eb(SUB["E25"], SUB["nu"]), 1.15, SUB["a"])
    lay2 = [(eb(hc["E_GPa"], hc["nu"]), 0.10, hc["alpha_ppm"]*1e-6), (eb(Si["E"], Si["nu"]), 0.10, Si["a"]),
            (eb(hm["E_GPa"], hm["nu"]), 0.025, hm["alpha_ppm"]*1e-6), (eb(Si["E"], Si["nu"]), 0.40, Si["a"])]
    c, b, k = hsueh(sub2, lay2, dT=Tend-Tsf_i)
    out["SC1_I25D_B0"] = dict(kappa_per_mm=k, c=c, b_mm=b, Scc_um_spherical_40mm=k*L**2/4*1e3, dT=Tend-Tsf_i)
    hcr = hom["I25D_C4_UF"]; hmr = hom["I25D_UBUMP_UF"]
    eth2 = [SUB["a"]*(Tend-Tsf_i),
            eth_bilinear(Tend, Tsf_i, hcr["glassy"]["alpha_ppm"]*1e-6, hcr["rubbery"]["alpha_ppm"]*1e-6, hcr["Tg"]),
            Si["a"]*(Tend-Tsf_i),
            eth_bilinear(Tend, Tsf_i, hmr["glassy"]["alpha_ppm"]*1e-6, hmr["rubbery"]["alpha_ppm"]*1e-6, hmr["Tg"]),
            Si["a"]*(Tend-Tsf_i)]
    c, b, k = hsueh(sub2, lay2, eth=eth2)
    out["SC1_I25D_B1_25C"] = dict(kappa_per_mm=k, Scc_um_spherical_40mm=k*L**2/4*1e3)

    # ---- 表
    temps = [25, 50, 75, 100, 110, 120, 122, 130, 135, 140, 143, 150, 165, 180, 200, 220, 245, 260]
    out["E_tables"] = {
        "UF_C4": poly_E_table(UF_C4["Eg"], UF_C4["Er"], UF_C4["Tg"]),
        "UF_MB": poly_E_table(UF_MB["Eg"], UF_MB["Er"], UF_MB["Tg"]),
        "EMC": poly_E_table(EMC["Eel"], EMC["Einf"], EMC["Tg"]),
        "ADH": poly_E_table(3.3, 0.1, 165.0, Tlo=0.0, Thi=260.0),
        "FCBGA_BUMP_UF": poly_E_table(hom["FCBGA_BUMP_UF"]["glassy"]["E_GPa"], hom["FCBGA_BUMP_UF"]["rubbery"]["E_GPa"], 122.0),
        "I25D_C4_UF": poly_E_table(hom["I25D_C4_UF"]["glassy"]["E_GPa"], hom["I25D_C4_UF"]["rubbery"]["E_GPa"], 122.0),
        "I25D_UBUMP_UF": poly_E_table(hom["I25D_UBUMP_UF"]["glassy"]["E_GPa"], hom["I25D_UBUMP_UF"]["rubbery"]["E_GPa"], 135.0),
    }
    out["secant_CTE_ppm"] = {}
    for Tsf, tag in [(150.0, "FCBGA_Tsf150"), (165.0, "I25D_Tsf165")]:
        d = {}
        for name, (ag, ar, Tg) in {
            "UF_C4": (26e-6, 107e-6, 122.0), "UF_MB": (30e-6, 105e-6, 135.0), "EMC": (7e-6, 22e-6, 143.0),
            "ADH": (20e-6, 55e-6, 165.0),
            "FCBGA_BUMP_UF": (hom["FCBGA_BUMP_UF"]["glassy"]["alpha_ppm"]*1e-6, hom["FCBGA_BUMP_UF"]["rubbery"]["alpha_ppm"]*1e-6, 122.0),
            "I25D_C4_UF": (hom["I25D_C4_UF"]["glassy"]["alpha_ppm"]*1e-6, hom["I25D_C4_UF"]["rubbery"]["alpha_ppm"]*1e-6, 122.0),
            "I25D_UBUMP_UF": (hom["I25D_UBUMP_UF"]["glassy"]["alpha_ppm"]*1e-6, hom["I25D_UBUMP_UF"]["rubbery"]["alpha_ppm"]*1e-6, 135.0),
        }.items():
            d[name] = secant_table(Tsf, ag, ar, Tg, temps)
        out["secant_CTE_ppm"][tag] = d

    # ---- Prony
    pr = {}
    for name, E0, Einf in [("UF_C4", 7.9, 0.2), ("UF_MB", 7.5, 0.1),
                           ("FCBGA_BUMP_UF", hom["FCBGA_BUMP_UF"]["glassy"]["E_GPa"], hom["FCBGA_BUMP_UF"]["rubbery"]["E_GPa"]),
                           ("I25D_C4_UF", hom["I25D_C4_UF"]["glassy"]["E_GPa"], hom["I25D_C4_UF"]["rubbery"]["E_GPa"]),
                           ("I25D_UBUMP_UF", hom["I25D_UBUMP_UF"]["glassy"]["E_GPa"], hom["I25D_UBUMP_UF"]["rubbery"]["E_GPa"])]:
        terms, E0c, s = prony_scaled(E0, Einf)
        pr[name] = dict(E0_GPa=E0, Einf_GPa=Einf, sum_alpha=round(sum(t[0] for t in terms), 6), terms=terms)
    hu = hk["I25D_UBUMP_UF_KIM2016"]
    terms, _, _ = prony_scaled(hu["glassy"]["E_GPa"], hu["rubbery"]["E_GPa"])
    pr["I25D_UBUMP_UF_KIM2016"] = dict(E0_GPa=hu["glassy"]["E_GPa"], Einf_GPa=hu["rubbery"]["E_GPa"],
                                       sum_alpha=round(sum(t[0] for t in terms), 6), terms=terms,
                                       status="borrowed_shape_sensitivity_only")
    E0c = sum(CHEN_EI) + CHEN_EINF
    pr["EMC_Chen2022"] = dict(E0_GPa=round(E0c, 5), Einf_GPa=CHEN_EINF,
                              terms=[[round(e/E0c, 6), t] for e, t in zip(CHEN_EI, CHEN_TAU)],
                              sum_alpha=round(sum(CHEN_EI)/E0c, 6))
    out["prony"] = pr
    out["wlf_linear_slope_per_K"] = 2.45104e7/1.87386e8
    json.dump(out, sys.stdout, indent=1, ensure_ascii=False)

if __name__ == "__main__":
    main()
