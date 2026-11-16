# 技术决策记录（ADR）

本目录记录 chipcae 的架构决策。编号与草案中的 D1–D5 对应。

| 编号 | 标题 | 状态 |
| --- | --- | --- |
| [ADR-0001](ADR-0001-no-gpl-dependencies.md) | 不引入 GPL 依赖 | 已接受 |
| [ADR-0002](ADR-0002-cutcell-mesh-in-house.md) | cut-cell 网格全自研 | cut-cell 自研已定；是否完全不用 AMReX，提议中 |
| [ADR-0003](ADR-0003-conditional-mfem.md) | 有条件采用 MFEM | 已接受，待原型 P1 验证 |
| [ADR-0004](ADR-0004-coupling-layer-and-precice.md) | 自研耦合层与 preCICE 适配器 | 已接受 |
| [ADR-0005](ADR-0005-desktop-and-kernel-processes.md) | 桌面端与求解内核分进程 | 已接受 |

新增决策请按 `ADR-XXXX-slug.md` 命名，并在本表登记。
