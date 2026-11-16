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
