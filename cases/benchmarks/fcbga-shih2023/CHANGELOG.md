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
