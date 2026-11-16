# Changelog — interposer25d-kim2016

## v0.1-vv0.6（2026-11-16，Asia/Shanghai）

- **Hsueh 公式更正**：同 FCBGA 卡。SC-1 的 \(\kappa_\mathrm{ref}\) 由已更正式给出。
- **对称性（Kim 2016 核对）**：原文 Table 1 中 ASIC 为 18.5×18.6×0.4 mm（非正方），中介层 26.4×19.5×0.1 mm（矩形），并含 stiffener。正文写明「To take an advantage of the symmetric structure of the sample, a 3D half model was constructed」（Fig. 7），未给出可核验的双轴对称四存储块坐标。因此**不能把文献封装当作四分之一对称**。P 系列与 NLGEOM 开的档位用全模型（对照可用半模型，与原文一致）。自选的双轴对称四存储块布局（chosen）仅用于小变形、NLGEOM 关的 verification，且须另留全模型变体。chipcae 默认跑全模型。
- 用途分层、cut-cell 网格与 `F-UNALIGNED` 变体：同 FCBGA 卡（C4/微凸点底填圆角）。
