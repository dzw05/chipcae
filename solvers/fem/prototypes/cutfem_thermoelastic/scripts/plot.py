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
spectrum = read('gp_spectrum')
spectral = {(a['physics'], float(a['gamma_N']), float(a['gamma_g']), float(a['vf'])): a for a in spectrum}
for a in gp:
    a.update(spectral[(a['physics'], float(a['gamma_N']), float(a['gamma_g']), float(a['vf']))])
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
for gg in ['0.01', '0.1', '0.3', '1']:
    data = [a for a in strip if np.isclose(number(a, 'gamma_g'), float(gg)) and number(a, 'gamma_N') == 10]
    ax.loglog([number(a, 'h') for a in data], [number(a, 'rel_err_cut') for a in data], 'o-', label=f'cut GP={gg}')
fitted = [a for a in strip if np.isclose(number(a, 'gamma_g'), .1) and number(a, 'gamma_N') == 10]
ax.loglog([number(a, 'h') for a in fitted], [number(a, 'rel_err_fitted') for a in fitted], 's-', label='fitted')
ax.axhline(.02, ls='--', color='gray', label='2%')
ax.set(xlabel='h (m)', ylabel='relative curvature error', title='Bilayer cooldown: example parameters')
ax.grid(True, which='both', alpha=.3)
ax.legend()
fig.tight_layout()
fig.savefig(r / 'timoshenko.png', dpi=170)
plt.close(fig)

fig, axes = plt.subplots(3, 2, figsize=(12, 13))
for row_index, physics in enumerate(['thermal', 'elastic', 'elastic_dirichlet_cut']):
    for gg in [0, .03, .1, .3, 1, 3]:
        data = [a for a in gp if a['physics'] == physics and number(a, 'gamma_N') == 10 and np.isclose(number(a, 'gamma_g'), gg)]
        vf = [number(a, 'vf') for a in data]
        label = f'GP={gg:g}'
        axes[row_index, 0].loglog(vf, [number(a, 'jacobi_kappa') if a['indefinite'] == '0' else np.nan for a in data], 'o-', label=label)
        axes[row_index, 1].semilogx(vf, [number(a, 'amg_iterations') if a['amg_status'] == 'ok' and a['indefinite'] == '0' else np.nan for a in data], 'o-', label=label)
    for col, ax in enumerate(axes[row_index]):
        ax.set(xlabel='edge fraction vf (corner fraction = vf²)', ylabel='exact Jacobi-scaled kappa' if col == 0 else 'AMG-PCG iterations', title=physics + ', gamma_N=10')
        ax.grid(True, which='both', alpha=.3)
        ax.legend(fontsize=8)
fig.tight_layout()
fig.savefig(r / 'gp_sweep.png', dpi=170)
plt.close(fig)

info = json.loads((r / 'run_info.json').read_text()) if (r / 'run_info.json').exists() else {}
lines = ['# 实测结果', '', f"所有数值来自本目录 CSV；FP64、单进程、Release -O2。扫描参数预先固定，默认值依据实测结果选择。hypre={info.get('hypre', 'unknown')}；构建和全部数值实验耗时 {info.get('seconds_before_plot', 'unknown')} s（未含绘图/许可检查）。", '', '## Manufactured solutions', '']
for title, rows in [('热传导（Dirichlet / Robin）', thermal), ('热弹性（GP on / off）', elastic), ('双材料热传导（k=1 / 10）', bimat)]:
    lines += ['### ' + title, '', table(rows, ['N', 'variant', 'gamma_N', 'gamma_g', 'L2', 'H1', 'order_L2', 'order_H1', 'residual', 'status']), '']
lines += ['## 双层条带：example parameters', '', 'y 向上，曲率定义为 uy 的二阶 x 导数。底层冷却收缩更多，截面关系 eps_xx=u0′−y uy″ 给出负曲率，移除刚体位移/转动后两端向下。以下参考值已转换到这一符号约定。', '', table(strip, ['Nt', 'gamma_N', 'gamma_g', 'kappa_cut', 'kappa_fitted', 'kappa_ref', 'rel_err_cut', 'rel_err_fitted', 'warpage_cut', 'thermal_error_cut']), '']
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
lines += ['', '## Ghost penalty 条件数扫描', '', '背景 64×16，顶边和右端的边长比例为 vf，角单元体积分数 vf²。elastic 在左侧夹持，elastic_dirichlet_cut 在右侧切割边夹持。全谱由对称矩阵的 numpy.linalg.eigvalsh 计算；非正最小特征值标记为非 SPD，Jacobi 对角不全为正时缩放谱不可用。Lanczos 列为 RHS 依赖的估计。', '', table(gp, ['physics', 'vf', 'gamma_N', 'gamma_g', 'jacobi_iterations', 'jacobi_kappa_estimate', 'jacobi_status', 'cg_kappa_estimate', 'amg_iterations', 'amg_status', 'lambda_min', 'lambda_max', 'kappa', 'jacobi_kappa', 'negative_eigenvalues', 'indefinite']), '', '## 验收指标：ghost penalty 系数 10 倍范围、vf 0.5→1e-4 的迭代数波动', '', '比值为对应范围内所有 vf 和网格 gamma_g 的最大迭代数 / 最小迭代数；只在所有矩阵 SPD 且对应求解均成功时给出，否则标记不可用。AMG 判据为比值 ≤ 2。', '']
acceptance = []
for physics in ['thermal', 'elastic', 'elastic_dirichlet_cut']:
    for gn in [10, 20, 40]:
        for low in [.03, .1, .3]:
            subset = [a for a in gp if a['physics'] == physics and number(a, 'gamma_N') == gn and low * (1-1e-9) <= number(a, 'gamma_g') <= 10 * low * (1+1e-9)]
            spd = all(a['indefinite'] == '0' for a in subset)
            def ratio(prefix):
                if not spd or any(a[prefix + '_status'] != 'ok' for a in subset):
                    return 'N/A'
                counts = [number(a, prefix + '_iterations') for a in subset]
                return max(counts) / min(counts) if min(counts) > 0 else 'N/A'
            amg, jac = ratio('amg'), ratio('jacobi')
            acceptance.append(dict(physics=physics, gamma_N=gn, gamma_g_range=f'{low:g}–{10*low:g}', all_spd='yes' if spd else 'no', amg_ratio=amg, amg_le_2=('yes' if amg <= 2 else 'no') if isinstance(amg, float) else 'N/A', jacobi_ratio=jac))
lines += [table(acceptance, ['physics', 'gamma_N', 'gamma_g_range', 'all_spd', 'amg_ratio', 'amg_le_2', 'jacobi_ratio']), '', '## Notes', '']
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
candidates = []
for gn in [10, 20, 40]:
    for gg in [.01, .1, .3, 1.]:
        subset = [a for a in gp if number(a, 'gamma_N') == gn and np.isclose(number(a, 'gamma_g'), gg)]
        accuracy = [a for a in strip if number(a, 'gamma_N') == gn and np.isclose(number(a, 'gamma_g'), gg)]
        if subset and accuracy and all(a['indefinite'] == '0' for a in subset):
            finest_accuracy = max(accuracy, key=lambda a: int(a['Nt']))
            candidates.append((number(finest_accuracy, 'rel_err_cut'), gn, gg, finest_accuracy))
if candidates:
    err, gn, gg, sample = min(candidates)
    lines.append(f'- 在已测参数中，所有物理和 vf 均 SPD 且最细条带曲率相对误差最小的组合为 gamma_N={gn:g}, gamma_g={gg:g}（Nt={sample["Nt"]} 误差 {100*err:.5g}%）；建议作为稳定默认候选。粗网格误差及迭代波动见表，精度/稳定性存在权衡。')
else:
    lines.append('- 已测条带精度参数中没有覆盖所有扫描物理与 vf 的 SPD 组合，无法据此推荐稳定默认值。')
for nt in [4, 32]:
    comparison = [a for a in strip if int(a['Nt']) == nt and number(a, 'gamma_N') == 10 and number(a, 'gamma_g') in (.1, .3)]
    if len(comparison) == 2:
        by_g = {number(a, 'gamma_g'): number(a, 'rel_err_cut') for a in comparison}
        lines.append(f'- 条带 Nt={nt}、gamma_N=10 时，gamma_g=0.1 与 0.3 的曲率相对误差分别为 {100*by_g[.1]:.5g}% 和 {100*by_g[.3]:.5g}%；条带为自由边和刚体约束，Nitsche 参数不影响此例。')
lines.append('- Lanczos 条件数估计依赖右端和停止阈值，不能替代全谱。')
lines.append(f"- 均匀冷却热解最大误差：{max(number(a, 'thermal_error_cut') for a in strip):.6g} K (cut)，{max(number(a, 'thermal_error_fitted') for a in strip):.6g} K (fitted)。")
if all(a['amg_status'] == 'unavailable' for a in gp):
    lines.append('- 本次未启用 hypre；AMG 列明确标为 unavailable。使用 ./run_all.sh --with-hypre 可重跑。')
else:
    lines.append('- 本次启用了 hypre 3.2.1；不定矩阵跳过 AMG，其他矩阵实测 AMG-PCG 迭代数见表。')
lines += ['- 未上 Kokkos；2D Q1、FP64、serial，3D 未实现。递归积分采用角点采样的分片线性 level set；小于子单元且角点不可见的封闭几何不在保证范围内。', f"- 实际默认 gamma_N={info.get('gamma_N')}、gamma_g={info.get('gamma_g_default')}；MMS 表记录实际参数。", '']
(r / 'summary.md').write_text('\n'.join(lines))
# End-to-end checks enforce the requested asymptotic convergence for the stable path.
for rows, variant in [(thermal, 'dirichlet'), (thermal, 'robin'), (elastic, 'gp_on'), (bimat, 'bimat')]:
    last = [a for a in rows if a['variant'] == variant][-1]
    if int(last['N']) >= 64:
        assert 1.7 < number(last, 'order_L2') < 2.3, (variant, last)
        assert .8 < number(last, 'order_H1') < 1.2, (variant, last)
print('Plots and measured summary written to', r)
print(f"Finest curvature: cut={number(finest, 'kappa_cut'):.8g}, fitted={number(finest, 'kappa_fitted'):.8g}, reference={number(finest, 'kappa_ref'):.8g} 1/m")
