# 技术决策记录（ADR）

本目录记录 chipcae 的架构决策。ADR-0001..0005 保持原编号，对应草案 D1–D5。

| 编号 | 标题 | 状态 |
| --- | --- | --- |
| [ADR-0001](ADR-0001-no-gpl-dependencies.md) | 不引入 GPL 依赖 | 已接受 |
| [ADR-0002](ADR-0002-cutcell-mesh-in-house.md) | cut-cell 网格全自研 | cut-cell 自研已定；是否完全不用 AMReX，提议中 |
| [ADR-0003](ADR-0003-conditional-mfem.md) | 有条件采用 MFEM | 已接受，待原型 P1 验证 |
| [ADR-0004](ADR-0004-coupling-layer-and-precice.md) | 自研耦合层与 preCICE 适配器 | 已接受 |
| [ADR-0005](ADR-0005-desktop-and-kernel-processes.md) | 桌面端与求解内核分进程 | 已接受 |
| [ADR-0006](ADR-0006-review-process.md) | AI 交叉评审与 owner 把关 | 已接受 |
| [ADR-0007](ADR-0007-step-import-occt.md) | STEP 导入采用 OCCT | 已接受 |
| [ADR-0008](ADR-0008-tech-stack.md) | 技术栈 | 已接受 |
| [ADR-0009](ADR-0009-ecad-step-import.md) | ECAD 与 STEP 导入阈值 | 已接受 |

新增决策请按 `ADR-XXXX-slug.md` 命名，并在本表登记。

## 仓库 ADR 编号 ↔ 架构草案编号/来源

| 仓库 ADR | 架构草案编号/来源 | 状态 |
| --- | --- | --- |
| ADR-0001 | D1 | 见上表（保留原状态） |
| ADR-0002 | D2 | 见上表（保留原状态） |
| ADR-0003 | D3 | 见上表（保留原状态） |
| ADR-0004 | D4 | 见上表（保留原状态） |
| ADR-0005 | D5 | 见上表（保留原状态） |
| [ADR-0006](ADR-0006-review-process.md) | 审查流程 | 已接受 |
| [ADR-0007](ADR-0007-step-import-occt.md) | STEP/OCCT | 已接受 |
| [ADR-0008](ADR-0008-tech-stack.md) | 技术栈 | 已接受 |
| [ADR-0009](ADR-0009-ecad-step-import.md) | ECAD 与 STEP 导入阈值 | 已接受 |

架构草案 ADR-014（作业层接口契约）尚无仓库 ADR；契约接受后再分配仓库编号。
