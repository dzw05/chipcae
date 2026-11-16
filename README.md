# chipcae（芯仿）

面向芯片与电子行业的多物理场仿真软件。旗舰场景是封装翘曲，产品路线是热-力一体化。

本仓库是单一大仓库：求解内核、桌面界面和通信协议放在一起。软件以私有闭源方式开发，不得引入 GPL 依赖。

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
| `.github/workflows/` | CI：快速检查，以及云端 MPI/GPU 占位工作流 |

## 许可

Proprietary, all rights reserved。见 [LICENSE](LICENSE)。
