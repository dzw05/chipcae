# Changelog — interposer25d-kim2016

## v0.1-vv0.6（2026-11-16，Asia/Shanghai）

- **Hsueh 公式更正**：同 FCBGA 卡。SC-1 的 \(\kappa_\mathrm{ref}\) 由已更正式给出。
- **对称性（Kim 2016 核对）**：原文 Table 1 中 ASIC 为 18.5×18.6×0.4 mm（非正方），中介层 26.4×19.5×0.1 mm（矩形），并含 stiffener。正文写明「To take an advantage of the symmetric structure of the sample, a 3D half model was constructed」（Fig. 7），未给出可核验的双轴对称四存储块坐标。因此**不能把文献封装当作四分之一对称**。P 系列与 NLGEOM 开的档位用全模型（对照可用半模型，与原文一致）。自选的双轴对称四存储块布局（chosen）仅用于小变形、NLGEOM 关的 verification，且须另留全模型变体。chipcae 默认跑全模型。
- 用途分层、cut-cell 网格与 `F-UNALIGNED` 变体：同 FCBGA 卡（C4/微凸点底填圆角）。

## v0.1-vv0.6.1（2026-11-16，Asia/Shanghai，按 PR #6 架构评审必改项）

- `materials/SUB_ISO.csv`：E = 25.0 GPa 一点的温度 438.15 K → 423.15 K（150 °C，Kim 2016 Table 3，p.238），与 `SUB_ISO.meta.yaml` 一致。
- `I25D_C4_UF`：写入 25 项 Prony（原 `terms: []`）；源改为 `tools/vv/derive_cards.py`；新增 `p_series_sensitivity`（E0/E∞ 端点 derived，WLF 区间 tbd）；c_s = 0.10 改记为 cited（Kim 2016 p.239）。
- 新增 `I25D_UBUMP_UF` 材料卡（均质弹性/CTE、25 项 Prony、provenance、灵敏度约定）。
- `F-UNALIGNED.qoi` 补 `S_cc`；新增 `qoi_definitions`（S_cc、P1、P2 路径）。
- 新增 `SI_KIM2016_{INTERPOSER,ASIC,MEMORY}`；`SI`（Chen 2022）标为仅 X/PW。
- `geometry.stiffener`：材料与贴装温度为文献值，尺寸 tbd（Kim 2016 未给）。
- `series.P-KIM2016.parameters`：列出全部文献 / 借用 / chosen / tbd 项。
