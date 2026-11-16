# 材料卡（CSV + 元数据）
# 按《材料本构接口规范 v0.2.2》的存储意图编写：元数据声明 SI、provenance、source。
# CSV 便于表格式浏览；与 Schema v1 的字段对应关系写在 sidecar YAML。
# 规范原文：https://cursor.com/canvas/store-cb1250dc-2b01-44d2-a2e6-225d0cab8b5b/526444ad-bcc5-475c-bfe4-50b6de310c6e
# 若字段与正式 Schema 有出入，以规范页为准，本目录为合理模板并在此注明。
# materials/

CSV 加 sidecar YAML，按材料本构接口规范 v0.2.2 的 SI 存储意图编写（见规范页）。
若与正式 Schema 字段有出入，以规范页为准；本目录是合理模板，特此注明。

P 系列不得使用 `I25D_C4_UF / I25D_UBUMP_UF` 中借自 EMC 的 Prony/WLF。

- `I25D_C4_UF`、`I25D_UBUMP_UF`：25 项 Prony 已写入卡内，可由 `tools/vv/derive_cards.py` 复现；P 系列灵敏度区间见各卡 `p_series_sensitivity`。
- `SI`：Chen 2022 Table 7，仅 X/PW verification。
- `SI_KIM2016_INTERPOSER / _ASIC / _MEMORY`：Kim 2016 Table 3（p.238），仅 P-KIM2016。
- `I25D_UBUMP_UF_KIM2016`：P-KIM2016 微凸点/底填均质层，组分全取 Kim 2016 Table 3（Cu 125 GPa / ν 0.3 / 16.5 ppm；底填 for ASIC and memory），φ = 0.03（「less than 3%」上限，chosen），式 (1)–(4)，由 `tools/vv/derive_cards.py` 复现。`I25D_UBUMP_UF`（Chen 2022 Cu）仅 X/PW。
- P-KIM2016 的 C4/底填均质层（SnAg，φ = 0.10）：**tbd**。Kim Table 3 的 SnAg E(T)「−74.995 T + 52,582」转引 Lau 1996，单位与温度标度未注明，原文未核对，不进冻结卡。`I25D_C4_UF`（SAC305，Depiver 2024，chosen）仅 X/PW。
- 均质化默认 Kim/Park 式 (1)–(4) 面内等效、各向同性用法；Voigt 横观各向同性只作备选卡（未生成，G_xz、ν_xy tbd）；Halpin–Tsai 不作默认。

数值写法：所有卡一律写成带符号指数（`3.10e+10`、`-2.45104e+7`），见 `tests/vv/test_yaml_numeric.py`。

Prony 形状缩放 s（P 系列灵敏度）：`tools/vv/prony_scale.py` 在加载前生成合法 Schema v1 卡（s 不是运行时字段）。只缩放 E_i、τ_i 不变；E0 不变，E∞(s) = E0 − s(E0 − E∞_ref)。报告三个端点：s = 1、s = 0（E ≡ E0）、E ≡ E∞；中间值不是默认本构。`constant_poisson_from_E` 为近似。
