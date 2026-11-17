#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
"""四分之一模型结构化 hex20 网格的单元数估计（嵌套加密：n_Lk = 2^(k-1) * n_L1）。"""
import math, json

def n_int(bps, h):
    """bps: 递增断点（含 0 与外边界）；每段 ceil(len/h) 个单元。"""
    return [max(1, math.ceil((b-a)/h - 1e-9)) for a, b in zip(bps[:-1], bps[1:])]

def count(xb, yb, regions, h1, nz1):
    """regions: [(x0,x1,y0,y1, [layer names])]，层的厚度方向单元数取 nz1[name]（L1）。"""
    res = {}
    for k in (1, 2, 3):
        f = 2**(k-1)
        nx = [n*f for n in n_int(xb, h1)]; ny = [n*f for n in n_int(yb, h1)]
        tot = 0
        for (x0, x1, y0, y1, layers) in regions:
            cx = sum(n for n, a, b in zip(nx, xb[:-1], xb[1:]) if a >= x0-1e-9 and b <= x1+1e-9)
            cy = sum(n for n, a, b in zip(ny, yb[:-1], yb[1:]) if a >= y0-1e-9 and b <= y1+1e-9)
            tot += cx*cy*sum(nz1[l]*f for l in layers)
        res[f"L{k}"] = dict(h_inplane_mm=h1/f, hex20=tot, approx_nodes=int(tot*4.3), approx_dof=int(tot*4.3*3))
    return res

out = {}
# FCBGA-A lidless quarter: die half 7.75, substrate half 20
xb = [0, 7.75, 20.0]
nz = dict(sub=2, bump=2, die=2)
out["fcbga_A_lidless"] = count(xb, xb, [(0, 20, 0, 20, ["sub"]), (0, 7.75, 0, 7.75, ["bump", "die"])], 0.8, nz)
# FCBGA-A + lid L1: foot ring x in [17,20]
xb = [0, 7.75, 17.0, 20.0]
nz = dict(sub=2, bump=2, die=2, tim=2, adh=2, foot=2, top=2)
reg = [(0, 20, 0, 20, ["sub"]), (0, 7.75, 0, 7.75, ["bump", "die", "tim"]),
       (17, 20, 0, 20, ["adh", "foot"]), (0, 17, 17, 20, ["adh", "foot"]), (0, 20, 0, 20, ["top"])]
out["fcbga_A_lid"] = count(xb, xb, reg, 0.8, nz)
# 2.5D-A quarter, no mold
xb = [0, 7.0, 7.375, 11.375, 13.2, 20.0]
yb = [0, 0.5, 8.5, 8.75, 9.75, 20.0]
nz = dict(sub=2, c4=2, ip=2, ub=2, soc=2, mem=2, emc=2)
reg = [(0, 20, 0, 20, ["sub"]), (0, 13.2, 0, 9.75, ["c4", "ip"]), (0, 7.0, 0, 8.75, ["ub", "soc"]),
       (7.375, 11.375, 0.5, 8.5, ["ub", "mem"])]
out["i25d_A_nomold"] = count(xb, yb, reg, 0.8, nz)
print(json.dumps(out, indent=1))
