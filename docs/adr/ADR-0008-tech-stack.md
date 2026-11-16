# ADR-0008：技术栈

- 状态：已接受
- 日期：2026-11-16

## 背景

需要明确内核与界面的依赖边界。决策来源：owner 技术栈与可选 MUMPS 后端决定。

## 决策

采用 C++20 + CMake、Kokkos + MPI、Hypre 与 PETSc、jsoncons、上游 gRPC C++（不用 Qt GRPC）、nanobind、Qt6 Widgets。内核 I/O 只用 HDF5，直接写 VTKHDF，不依赖 VTK；VTK 9.7 仅用于 GUI/preview。MFEM 仍以 ADR-0003 的 P1 验证为前提。MUMPS（CeCILL-C）仅为可选、默认 OFF、动态链接的直接求解后端，不是必选技术栈。

## 后果

可选依赖默认关闭；依赖边界与条件必须在构建和验收中保持。
