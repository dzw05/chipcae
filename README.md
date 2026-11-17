# chipcae（芯仿）

面向芯片与电子行业的多物理场仿真软件。旗舰场景是封装翘曲，产品路线是热-力一体化。

本仓库是单一大仓库：求解内核、桌面界面和通信协议放在一起。源码公开，仅供查阅，不得引入 GPL 依赖。

## 当前状态

工程处于骨架阶段（M0）。默认配置在不安装 Kokkos、MFEM、Qt 等可选依赖的情况下即可编译并运行单元测试。

## 构建

需要 C++20 编译器和 CMake 3.20 或更高版本。

```bash
cmake -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

常用选项（默认均为 `OFF`，测试默认 `ON`）：

| 选项 | 含义 |
| --- | --- |
| `CHIPCAE_ENABLE_KOKKOS` | 启用 Kokkos |
| `CHIPCAE_ENABLE_MPI` | 启用 MPI |
| `CHIPCAE_ENABLE_MFEM` | 启用 MFEM（有条件采用，待原型 P1 验证） |
| `CHIPCAE_ENABLE_HDF5` | 启用 HDF5 / VTKHDF |
| `CHIPCAE_ENABLE_HYPRE` / `CHIPCAE_ENABLE_PETSC` | 线性求解器 |
| `CHIPCAE_ENABLE_GRPC` | 生成 gRPC / Protobuf 代码 |
| `CHIPCAE_BUILD_GUI` | 构建 Qt6 桌面端 |
| `CHIPCAE_ENABLE_PYTHON` | 构建 Python 绑定 |
| `CHIPCAE_BUILD_TESTS` | 构建测试 |

Python 绑定使用 nanobind（需要 Python 3.9+ 开发头文件）。
同时开启 MPI 与 HDF5 时必须使用并行 HDF5，每个输出步集体写一个 VTKHDF 文件。
`CHIPCAE_ENABLE_PRECICE`（v2 适配器）、`CHIPCAE_ENABLE_PARTICLES`（内核粒子链接）默认 OFF；`CHIPCAE_WARNINGS_AS_ERRORS` 默认 OFF，CI 启用。

## 仓库结构

| 目录 | 职责 |
| --- | --- |
| `core/` | 基础类型、Field、并行抽象、日志、参数声明 |
| `geometry/` | STL/STEP 导入、符号距离场（SDF） |
| `mesh/` | cut-cell 网格、分层六面体网格生成 |
| `solvers/fvm/` | 流动与共轭传热 |
| `solvers/fem/` | 热应力与翘曲 |
| `solvers/particles/` | MPM/SPH，目前只预留接口 |
| `coupling/` | 耦合器、场映射、preCICE 适配器 |
| `io/` | HDF5/VTKHDF 写出、结果清单、检查点 |
| `protocol/` | 界面与内核的通信协议，含错误码表 |
| `kernel/` | 求解进程入口 |
| `jobmgr/` | 作业管理服务 |
| `preview/` | 预览子进程 |
| `gui/` | Qt6 桌面端 |
| `python/` | Python 绑定 |
| `schema/` | 导出的参数 Schema 快照 |
| `cases/` | 基准算例与参考数据 |
| `tests/` | 单元测试、集成测试、验证测试 |
| `docs/adr/` | 技术决策记录 |
| `cmake/` | CMake 模块与许可证策略 |
| `.github/workflows/` | 托管 CI、夜间额外配置，以及默认关闭的 Tier B |
| `bench/` | 独立性能微基准，不进 CMake |
| `tools/perf/` | 重复次数、中位数和基线比较 |
| `tools/consistency/` | CPU / GPU 结果比较 |
| `docs/ci.md` | 分钟预算、CI 门禁和 Tier B 开关 |

## 持续集成

唯一的硬门禁是汇总作业 **`CI / ci-ok`**（ADR-0006）。lint 必须通过；`CI / Build and test` 在成功或被跳过（推送到 `main`、只改文档的 PR）时都算通过。草稿上 lint 会跳过，`ci-ok` 为失败。不要开启分支保护，也不要把单项检查设为必选。

托管分钟预算、Tier B 开关（`TIER_B_ENABLED`）和性能门槛写在 [docs/ci.md](docs/ci.md)。仓库设置没有在这个改动里动过。

## 许可

Copyright (c) 2026 Zhiwei Duan
All rights reserved. Source is published for viewing only; no permission is granted to use, copy, modify, merge, publish, distribute, sublicense, or sell without prior written permission. See [LICENSE](LICENSE) for full terms and separately licensed exceptions.

保留所有权利，源码公开仅供查阅；未经事先书面许可，不得使用、复制、修改、合并、发布、分发、再许可或销售。完整条款及单独许可的例外见 [LICENSE](LICENSE)。

O2 求解器当前限制：每个 MPI rank 均在 `PETSC_COMM_SELF` 上对全局串行网格执行完整求解。
不同 `np` 的结果一致仅验证重复求解及输出一致性，不证明分布式求解正确；总内存随 `np` 增长。
分布式求解将随 GMG 落地。
