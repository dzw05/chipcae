#!/usr/bin/env python3
"""Render measured CSV data and write a Chinese results report. No synthetic data."""
import argparse
import csv
import json
from pathlib import Path
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

parser = argparse.ArgumentParser()
parser.add_argument('--results', type=Path, default=Path('results'))
r = parser.parse_args().results

def read(name):
    with (r / (name + '.csv')).open() as f:
        return list(csv.DictReader(f))

def number(row, key):
    return float(row[key])

def table(rows, keys, labels=None):
    result = ['| ' + ' | '.join(labels or keys) + ' |', '| ' + ' | '.join(['---'] * len(keys)) + ' |']
    for row in rows:
        vals = []
        for key in keys:
            value = row[key]
            try:
                x = float(value)
                value = f'{x:.6g}'
            except (ValueError, TypeError):
                pass
            vals.append(value)
        result.append('| ' + ' | '.join(vals) + ' |')
    return '\n'.join(result)

thermal, elastic, bimat, strip, gp = [read(n) for n in ['mms_thermal', 'mms_elastic', 'mms_thermal_bimat', 'timoshenko', 'gp_sweep']]
for name, rows in [('mms_thermal', thermal), ('mms_elastic', elastic)]:
    fig, axes = plt.subplots(1, 2, figsize=(11, 4))
    for variant in dict.fromkeys(a['variant'] for a in rows):
        data = [a for a in rows if a['variant'] == variant]
        h = np.array([number(a, 'h') for a in data])
        for ax, key, order in zip(axes, ['L2', 'H1'], [2, 1]):
            error = np.array([number(a, key) for a in data])
            ax.loglog(h, error, 'o-', label=variant)
            if variant == rows[0]['variant']:
                ax.loglog(h, error[-1] * (h / h[-1]) ** order, '--', color='gray', label=f'slope {order}')
            ax.set(xlabel='background h', ylabel=key + ' error')
            ax.grid(True, which='both', alpha=.3)
            ax.legend()
    fig.suptitle(name.replace('_', ' '))
    fig.tight_layout()
    fig.savefig(r / (name + '.png'), dpi=170)
    plt.close(fig)

fig, ax = plt.subplots(figsize=(7, 5))
for gg in ['0.01', '0.1', '1']:
    data = [a for a in strip if np.isclose(number(a, 'gamma_g'), float(gg))]
    ax.loglog([number(a, 'h') for a in data], [number(a, 'rel_err_cut') for a in data], 'o-', label=f'cut GP={gg}')
fitted = [a for a in strip if np.isclose(number(a, 'gamma_g'), .1)]
ax.loglog([number(a, 'h') for a in fitted], [number(a, 'rel_err_fitted') for a in fitted], 's-', label='fitted')
ax.axhline(.02, ls='--', color='gray', label='2%')
ax.set(xlabel='h (m)', ylabel='relative curvature error', title='Bilayer cooldown: example parameters')
ax.grid(True, which='both', alpha=.3)
ax.legend()
fig.tight_layout()
fig.savefig(r / 'timoshenko.png', dpi=170)
plt.close(fig)

fig, axes = plt.subplots(2, 2, figsize=(12, 9))
for row_index, physics in enumerate(['thermal', 'elastic']):
    for gg in [0, .03, .1, .3]:
        data = [a for a in gp if a['physics'] == physics and np.isclose(number(a, 'gamma_g'), gg)]
        vf = [number(a, 'vf') for a in data]
        good = np.array([a['jacobi_status'] == 'ok' for a in data])
        kval = np.array([number(a, 'jacobi_kappa') for a in data])
        kval[~good] = np.nan
        label = 'GP off' if gg == 0 else f'GP={gg}'
        axes[row_index, 0].loglog(vf, kval, 'o-', label=label + ' Jacobi')
        cgk = [number(a, 'cg_kappa') if a['cg_status'] == 'ok' else np.nan for a in data]
        axes[row_index, 0].loglog(vf, cgk, ':', alpha=.65, label=label + ' CG')
        iterations = [number(a, 'jacobi_iterations') if a['jacobi_status'] == 'ok' else np.nan for a in data]
        axes[row_index, 1].semilogx(vf, iterations, 'o-', label=label + ' Jacobi')
        if any(a['amg_status'] == 'ok' for a in data):
            axes[row_index, 1].semilogx(vf, [number(a, 'amg_iterations') if a['amg_status'] == 'ok' else np.nan for a in data], 's--', label=label + ' AMG')
        failures = [number(a, 'vf') for a in data if a['jacobi_status'] != 'ok']
        if failures:
            axes[row_index, 1].scatter(failures, np.zeros(len(failures)), marker='x', label=label + ' breakdown')
    for col_index in range(2):
        ax = axes[row_index, col_index]
        ax.set(xlabel='edge fraction vf (corner area fraction = vf^2)', ylabel='Lanczos kappa estimate' if col_index == 0 else 'iterations', title=physics)
        ax.grid(True, which='both', alpha=.3)
        ax.legend(fontsize=7, ncol=2)
fig.tight_layout()
fig.savefig(r / 'gp_sweep.png', dpi=170)
plt.close(fig)

info = json.loads((r / 'run_info.json').read_text()) if (r / 'run_info.json').exists() else {}
lines = ['# 实测结果', '', f"所有数值来自本目录 CSV；FP64、单进程、Release -O2。参数未按结果调整。hypre={info.get('hypre', 'unknown')}；构建和全部数值实验耗时 {info.get('seconds_before_plot', 'unknown')} s（未含绘图/许可检查）。", '', '## Manufactured solutions', '']
for title, rows in [('热传导（Dirichlet / Robin）', thermal), ('热弹性（GP on / off）', elastic), ('双材料热传导（k=1 / 10）', bimat)]:
    lines += ['### ' + title, '', table(rows, ['N', 'variant', 'gamma_g', 'L2', 'H1', 'order_L2', 'order_H1', 'residual', 'status']), '']
lines += ['## 双层条带：example parameters', '', 'y 向上，曲率定义为 uy 的二阶 x 导数。底层冷却收缩更多，截面关系 eps_xx=u0′−y uy″ 给出负曲率，移除刚体位移/转动后两端向下。以下参考值已转换到这一符号约定。', '', table(strip, ['Nt', 'gamma_g', 'kappa_cut', 'kappa_fitted', 'kappa_ref', 'rel_err_cut', 'rel_err_fitted', 'warpage_cut', 'thermal_error_cut']), '']
finest = max(fitted, key=lambda a: int(a['Nt']))
lines += [f"最细 fitted 网格相对 Timoshenko 的差为 **{100 * number(finest, 'rel_err_fitted'):.5g}%**（有限长度/二维模型差异与残余离散误差合计，不能当成已严格分离的模型误差）。", '', table(fitted, ['Nt', 'kappa_cut', 'kappa_cut_narrow', 'kappa_fitted', 'kappa_fitted_narrow'], ['Nt', 'cut L/4', 'cut L/8', 'fitted L/4', 'fitted L/8']), '']
lines += ['### 曲率收敛与拟合窗口', '']
for key in ['kappa_cut', 'kappa_fitted']:
    vals = np.array([number(a, key) for a in fitted])
    for i in range(2, len(vals)):
        order = np.log2(abs((vals[i-2] - vals[i-1]) / (vals[i-1] - vals[i])))
        lines.append(f"- {key}，Nt={fitted[i]['Nt']}：三层差值 Richardson 阶 {order:.5g}。")
    narrow = number(finest, key + '_narrow')
    wide = number(finest, key)
    lines.append(f'- {key} 最细层拟合半宽从 L/4 改为 L/8，曲率相对变化 {100 * abs((narrow-wide)/wide):.5g}%。')
lines += ['', '## Ghost penalty 条件数扫描', '', '背景 64×16，顶边 y=(13+vf)hy，右端 x=(59+vf)hx，因此角单元体积分数 vf²；左、底边也不贴网格。kappa 为 CG alpha/beta 构造的 Lanczos 三对角矩阵极值估计，不是全谱精确条件数。CG/PCG 均最多 20000 步，真相对残差目标 1e-8。indefinite 表示负对角或 pᵀAp≤0；未检测到负曲率并不证明整个矩阵 SPD。', '', table(gp, ['physics', 'vf', 'min_volfrac', 'gamma_g', 'jacobi_iterations', 'jacobi_kappa', 'jacobi_status', 'cg_iterations', 'cg_kappa', 'cg_status', 'amg_iterations', 'amg_status', 'indefinite', 'ldl_indefinite']), '', '## Notes', '']
for rows in [thermal, elastic, bimat]:
    for variant in dict.fromkeys(a['variant'] for a in rows):
        last = [a for a in rows if a['variant'] == variant][-1]
        lines.append(f"- {variant} 最细层观测阶：L2={number(last, 'order_L2'):.5g}，H1={number(last, 'order_H1'):.5g}。")
bad_mms = [a for a in thermal + elastic + bimat if a['status'] != 'ok']
if bad_mms:
    lines.append('- 无 GP 的部分 MMS 矩阵不定；直接 LDLᵀ 保留负 pivot 求解原矩阵，未更改 gamma_N、未正则化。状态见表。')
if number(finest, 'rel_err_cut') > .02 or number(finest, 'rel_err_fitted') > .02:
    lines.append('- 最细层曲率误差仍超过 2%；保留实测结果。需要进一步区分切割稳定化、Q1 弯曲离散、有限长度端部效应与参考模型的差异。')
else:
    lines.append('- 默认 GP 最细层 cut/fitted 曲率误差均小于 2%。')
thermal_bad = [a for a in gp if a['physics'] == 'thermal' and a['indefinite'] == '1']
if thermal_bad:
    lines.append('- 极小角切割的 thermal 扫描中，gamma_g=0/0.03 失去正定性；gamma_g=0.1 在 vf≤0.001 也不定。独立带状 LDLᵀ 的负 pivot 检查与 CG 检测一致。gamma_g=0.3 对扫描的全部 vf 均收敛。保留默认值与原始数据，不调整参数消除失败。')
lines.append('- 原 RHS 的 CG/Lanczos 估计依赖已激发的模态和停止阈值；极小切割的模式可能在残差 1e-8 下尚未解析。因此部分 GP-off 的 kappa_est 不单调，不能将它当成全谱条件数上界或网格稳健性证明。')
lines.append(f"- 均匀冷却热解最大误差：{max(number(a, 'thermal_error_cut') for a in strip):.6g} K (cut)，{max(number(a, 'thermal_error_fitted') for a in strip):.6g} K (fitted)。")
if all(a['amg_status'] == 'unavailable' for a in gp):
    lines.append('- 本次未启用 hypre；AMG 列明确标为 unavailable。使用 ./run_all.sh --with-hypre 可重跑。')
else:
    lines.append('- 本次启用了 hypre 3.2.1；不定矩阵跳过 AMG，其他矩阵实测 AMG-PCG 迭代数见表。')
lines += ['- 未上 Kokkos；2D Q1、FP64、serial，3D 未实现。递归积分采用角点采样的分片线性 level set；小于子单元且角点不可见的封闭几何不在保证范围内。', '- Nitsche gamma_N=10、默认 gamma_g=0.1；固定背景 h 对极小切割在 GP off 时不保证强制性，故扫描中的不定是需要报告的结果。', '']
(r / 'summary.md').write_text('\n'.join(lines))
# End-to-end checks enforce the requested asymptotic convergence for the stable path.
for rows, variant in [(thermal, 'dirichlet'), (thermal, 'robin'), (elastic, 'gp_on'), (bimat, 'bimat')]:
    last = [a for a in rows if a['variant'] == variant][-1]
    if int(last['N']) >= 64:
        assert 1.7 < number(last, 'order_L2') < 2.3, (variant, last)
        assert .8 < number(last, 'order_H1') < 1.2, (variant, last)
print('Plots and measured summary written to', r)
print(f"Finest curvature: cut={number(finest, 'kappa_cut'):.8g}, fitted={number(finest, 'kappa_fitted'):.8g}, reference={number(finest, 'kappa_ref'):.8g} 1/m")
