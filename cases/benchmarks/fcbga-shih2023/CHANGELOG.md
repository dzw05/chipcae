# Changelog — fcbga-shih2023

## v0.1-vv0.6（2026-11-16，Asia/Shanghai）

- **Hsueh 公式更正**：上游 `warpage_benchmarks.md` §3.3 曾将弯曲轴 \(b\) 的分子误写为 \(-(E_s t_s^2+\sum\cdots)\)，层项符号与 Hsueh, Luttrell & Cui (2006) 式 (4b) 相反，会使曲率相对宽板 Timoshenko 偏离约 20%。正确分子为 \(-E_s t_s^2 + \sum E_i t_i(2h_{i-1}+t_i)\)。`tools/vv/derive_cards.py` 一直按正确式实现，并与 CLT 及二层 Timoshenko 交叉。Page 更正在 v0.5.1。
- 对称性：四分之一模型仅用于 NLGEOM 关闭的小变形外部对照；NLGEOM 打开的档位一律全模型；每个对称模型保留全模型变体；chipcae 默认跑全模型。
- 用途分层：X/PW 为 verification（允许 chosen 参数）；P 系列为 validation（仅文献参数；chosen/tbd 须显式列出；P 系列不得借用 EMC 的底填参数，改为灵敏度参数并给范围）。
- 网格：MVP 走 cut-cell 主路径。背景网格为 z 向对齐层界面的张量积笛卡尔网格；焊球、圆角、斜边用 cut。新增变体 `F-UNALIGNED`（底填圆角不与网格对齐）和对齐版比较。
- `benchmark.yaml` 为 V&V 规范格式 v0.1，不是求解器算例 schema。

## v0.1-vv0.6.1（2026-11-16，Asia/Shanghai，按 PR #6 架构评审必改项）

- `FCBGA_BUMP_UF`：写入 25 项 Prony（原 `terms: []`）；源改为 `tools/vv/derive_cards.py`；新增 `p_series_sensitivity`（E0/E∞ 端点 derived，WLF 区间 tbd）。
- `series.P-SHIH2023.parameters`：显式列出借用的 `SUB_ISO`（Kim 2016）与借用底填 Prony/WLF；补列 Shih 2023 Table 3 文献参数。

## v0.1-vv0.6.2（2026-11-17，Asia/Shanghai，连续介质求解器组核定项）

- **YAML 数值写法（bug）**：YAML 1.1 把不带符号的指数读成字符串。`SUB_ISO.meta` E 表、`SI.meta` E、`FCBGA_BUMP_UF` 的 `infinity` 与 WLF `C1/C2` 一律改为带符号指数（如 `3.10e10` → `3.10e+10`、`-2.45104e7` → `-2.45104e+7`）。新增 `tests/vv/test_yaml_numeric.py`：`yaml.safe_load` 全部卡，数值字段必须是 int/float。
- 新增 `SI_SHIH2023`（Shih 2023 Table 3 p.8 Die：131 GPa / ν 0.30 / 2.8 ppm/K），P-SHIH2023 专用；`SI`（Chen 2022，ν 0.28）仅 X/PW。
- P-SHIH2023 注明：基板与底填参数为借用，Shih 图 12 数字化前结果不算对 128 µm（Shih 2023 p.9 FE 预测值；实测约 122 µm）的复现；均质层不按 Kim 组分重推，改用 Shih 图 12 等效材料曲线（tbd）。
- **Prony 形状缩放 s**（P 系列灵敏度参数）：只缩放 E_i、τ_i 不变；E0 不变，E∞(s) = E0 − s(E0 − E∞_ref)，保持 E∞ + ΣE_i = E0，s = 0 即 E ≡ E0。新增 `tools/vv/prony_scale.py`，加载前生成合法 Schema v1 卡（s 不是运行时字段）；报告 s = 1、s = 0、E ≡ E∞ 三端点，中间值不是默认本构；`constant_poisson_from_E` 为近似。各卡 `p_series_sensitivity.prony_shape_scale_s` 已更新；新增 `tests/vv/test_prony_scale.py`。
- 合入 `main`（PR #7，含 `tools/__init__.py`）；新增 `tools/vv/__init__.py`，`tools.vv.*` 可按包导入。
