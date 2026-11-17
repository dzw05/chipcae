# 材料卡（CSV + 元数据）
# 按《材料本构接口规范 v0.2.2》的存储意图编写：元数据声明 SI、provenance、source。
# CSV 便于表格式浏览；与 Schema v1 的字段对应关系写在 sidecar YAML。
# 规范原文：https://cursor.com/canvas/store-cb1250dc-2b01-44d2-a2e6-225d0cab8b5b/526444ad-bcc5-475c-bfe4-50b6de310c6e
# 若字段与正式 Schema 有出入，以规范页为准，本目录为合理模板并在此注明。
# materials/

CSV 加 sidecar YAML，按材料本构接口规范 v0.2.2 的 SI 存储意图编写（见规范页）。
若与正式 Schema 字段有出入，以规范页为准；本目录是合理模板，特此注明。

P 系列不得使用 `FCBGA_BUMP_UF` 中借自 EMC 的 Prony/WLF。

- `FCBGA_BUMP_UF`：25 项 Prony 已写入卡内，可由 `tools/vv/derive_cards.py` 复现；P 系列灵敏度区间见卡内 `p_series_sensitivity`。
- `SI`：Chen 2022 Table 7（ν = 0.28），仅 X/PW verification。
- `SI_SHIH2023`：Shih 2023 Table 3（p.8）Die，131 GPa / ν 0.30 / 2.8 ppm/K，仅 P-SHIH2023。
- `SUB_ISO`：借自 Kim 2016 Table 3 基板，P-SHIH2023 中按「借用」列出（见 `benchmark.yaml` 的 `series.P-SHIH2023.parameters`）。

数值写法：YAML 1.1（PyYAML）把不带符号的指数（如 `3.10e10`）读成字符串，所有卡一律写成带符号指数（`3.10e+10`、`-2.45104e+7`）。`tests/vv/test_yaml_numeric.py` 检查。

Prony 形状缩放 s（P 系列灵敏度）：`tools/vv/prony_scale.py` 在加载前生成合法 Schema v1 卡（s 不是运行时字段）。只缩放 E_i、τ_i 不变；E0 不变，E∞(s) = E0 − s(E0 − E∞_ref)。报告三个端点：s = 1（借用形状）、s = 0（E ≡ E0）、E ≡ E∞；中间值不是默认本构。`constant_poisson_from_E` 为近似。
