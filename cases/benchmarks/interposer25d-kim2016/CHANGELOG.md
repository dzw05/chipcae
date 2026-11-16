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

## v0.1-vv0.6.2（2026-11-17，Asia/Shanghai，连续介质求解器组核定项）

- **YAML 数值写法（bug）**：YAML 1.1 把不带符号的指数读成字符串。`SUB_ISO.meta` E 表、`SI.meta` E、`I25D_C4_UF` 的 `infinity` 与 WLF `C1/C2` 一律改为带符号指数（如 `3.10e10` → `3.10e+10`、`-2.45104e7` → `-2.45104e+7`）。新增 `tests/vv/test_yaml_numeric.py`：`yaml.safe_load` 全部卡，数值字段必须是 int/float。
- **P-KIM2016 均质层按 Kim 2016 Table 3 组分重推**（式 (1)–(4)，`tools/vv/derive_cards.py` 的 `kim2016_homogenized`）：
  - 微凸点/底填层，新卡 `I25D_UBUMP_UF_KIM2016`（Cu 125 GPa / ν 0.3 / 16.5 ppm，原 Chen 2022 Cu 125 / 0.34 / 17 ppm；φ = 0.03 为「less than 3%」上限）：E 玻璃态 7.9281 → 7.9318 GPa，橡胶态 0.1130 → 0.1130 GPa；面内 α 30.8092 → 30.8511 / 127.2997 → 127.4291 ppm/K；α_z 25.5782 → 25.4082 / 19.2189 → 18.7315 ppm/K；ν 0.3012 → 0.3000。`I25D_UBUMP_UF` 保留，仅 X/PW。
  - C4/底填层（SnAg ν 0.4 / 21.5 ppm，φ = 0.10）：SnAg E(T) 转引 Lau 1996，单位与温度标度未注明，原文未核对 → **tbd**，不进冻结卡。`I25D_C4_UF`（SAC305，chosen）仅 X/PW，P-KIM2016 暂作借用并标明。
  - 默认 Kim/Park 面内等效；Voigt 横观各向同性只作备选卡（未生成）；Halpin–Tsai 不作默认。
- **Prony 形状缩放 s**（P 系列灵敏度参数）：只缩放 E_i、τ_i 不变；E0 不变，E∞(s) = E0 − s(E0 − E∞_ref)，保持 E∞ + ΣE_i = E0，s = 0 即 E ≡ E0。新增 `tools/vv/prony_scale.py`，加载前生成合法 Schema v1 卡（s 不是运行时字段）；报告 s = 1、s = 0、E ≡ E∞ 三端点，中间值不是默认本构；`constant_poisson_from_E` 为近似。各卡 `p_series_sensitivity.prony_shape_scale_s` 已更新；新增 `tests/vv/test_prony_scale.py`。
- 合入 `main`（PR #7，含 `tools/__init__.py`）；新增 `tools/vv/__init__.py`，`tools.vv.*` 可按包导入。
