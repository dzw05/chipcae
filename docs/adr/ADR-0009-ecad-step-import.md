# ADR-0009: ECAD 与 STEP 导入阈值

- 状态：已接受
- 日期：2026-11-17

## 背景

ECAD 与 STEP 导入需要共用一组可审查的尺度阈值，以决定焊接容差、缺陷处理等级和极薄层提示。阈值由 `geometry/thresholds/import.json` 集中保存。

## 决策

- 焊接容差为 `max(min(weld_rel_L × L, weld_rel_t × t_min), 10 × 10⁻⁹ m)`。
- A、B 级阈值分别取包络尺寸和最薄层厚度相对值的较小者。
- 层合并建议阈值为 `max(min(merge_rel_h × h_xy, merge_abs_max), 2 × dbu_m)`。
- 缺陷按 Weld、A、B、C 顺序分类；超过 B 级阈值为 C。
- 厚度为零是错误；厚度低于相邻层较小厚度的 `thin_ratio` 倍，或纵横比超过 `thin_aspect`，建议合并。
- 极薄层判定只报告建议，不执行几何修复。

## 后果

不同输入尺寸按同一组显式规则换算容差。阈值文件的变更须按 MS5 审批流程进行。
