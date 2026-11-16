# ADR-0001：不引入 GPL 依赖

- 状态：已接受
- 日期：2026-11-16
- 编号：D1

## 背景

chipcae 走闭源商用路线，目标用户是工业界工程师。若引入 GPL 依赖，会限制发行方式，并可能把传染性义务带到求解内核。

## 决策

第三方依赖必须使用宽松许可证（MIT、BSD、Apache 2.0 等），或在商业发行时可以动态链接的 LGPL。构建系统和 CI 中的许可证检查应拒绝 GPL 依赖。

当前允许列入依赖清单的库：

- 内核与数值：Kokkos、MPI、HDF5、Hypre、PETSc、jsoncons、gRPC / Protobuf
- 有限元底座：MFEM（见 ADR-0003，有条件采用）
- 界面：Qt6、VTK、Open CASCADE（OCCT）

明确排除 p4est、t8code、Basilisk 等 GPL 库。deal.II 本身是 LGPL，但其分布式网格依赖 p4est，因此只作为 MFEM 的备选，在确认可以避开 p4est 之前不得引入。

## 后果

- 选型空间变窄，但发行路径清晰。
- 新增依赖必须先核对许可证，再写入 CMake。
- `cmake/check_license_policy.py` 会在配置和 CI 中扫描构建脚本。
