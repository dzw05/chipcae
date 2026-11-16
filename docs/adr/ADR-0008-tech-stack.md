# ADR-0008：技术栈

- 状态：已接受
- 日期：2026-11-16

## 背景

需要明确内核与界面的依赖边界。决策来源：owner 技术栈与可选 MUMPS 后端决定。

## 决策

- 语言与构建：C++20 + CMake；可选依赖默认 OFF。
- **CPU 与 GPU 从 MVP 起同为一等路径，计算效率是核心指标**：
  - 节点内并行用 Kokkos（CPU：OpenMP/Serial；GPU：CUDA 优先，HIP 随后），跨节点用 MPI；
  - FEM 用 MFEM，开启 device 后端（以 ADR-0003 的 P1 验证为前提），与 Kokkos 只在指针层面共享数据；
  - 线性求解用 hypre 和 PETSc 的 GPU 构建（BoomerAMG / GAMG）；MUMPS（CeCILL-C）只作可选、默认 OFF、动态链接的直接求解后端；
  - 材料本构函数必须能在设备端调用（KOKKOS_FUNCTION / MFEM_HOST_DEVICE），默认 FP64；
  - 冻结接口里的数组用带显式内存空间参数的 Kokkos::View（默认 HostSpace），I/O 先拷到主机再写。
- 网格划分：METIS（串行，Apache-2.0），以后的并行重分区用 Zoltan；排除 ParMETIS。分层网格的 ColumnExtrudePartitioner 和 HaloPlan 放在 `chipcae::par`。
- I/O：内核只用 HDF5，直接写 VTKHDF（≥ 2.5，cut-cell 写多面体，保留全六面体回退），不依赖 VTK；每个输出步一个并行 HDF5 文件，集体写入，由 rank 0 登记 manifest。
- 协议与脚本：上游 gRPC C++（不用 Qt GRPC），jsoncons，nanobind。
- 界面：Qt6 Widgets，VTK 9.7 只用于 GUI 和 preview；OCCT 动态链接（ADR-0007）。
- 交付形态：MVP 是单机版；v1 增加私有服务器版（jobd + Slurm/PBS）；不做云 SaaS。

## 后果

可选依赖默认关闭；依赖边界与条件必须在构建和验收中保持。

CI 必须同时覆盖 CPU 构建和 GPU（CUDA）构建；GPU 测试和性能回归放在云上的 nightly 中跑。
