# io

结果写出、结果清单和检查点。

目标格式是 HDF5 / VTKHDF。在关闭 HDF5 依赖时，清单和检查点使用轻量文本占位实现，保证内核在最小配置下也能写出结果索引。

内核通过 HDF5 C API 直接写 VTKHDF UnstructuredGrid。写出版本下限为 VTKHDF 2.5（多面体）；共享面复用需要阅读端 ≥ 2.9，作为可选项，默认关闭。cut-cell 使用多面体单元，保留全六面体回退方案。

每个输出步一个并行 HDF5 文件，采用 collective writes。文件关闭后，rank 0 才将它登记到 JSON manifest；当前 manifest writer 仍为占位实现，带 `manifest_version` 和 `entries`，不代表完整契约。
