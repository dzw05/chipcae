# tests

单元测试、集成测试和验证测试。

- `unit/`：不依赖外部求解库的快速测试，在每个 PR 的 GitHub Actions 上运行。
- 集成测试和验证算例将逐步加入，并与 `cases/` 中的基准数据对应。
- 多节点 MPI 和 GPU 的大算例不在默认 CI 中运行，见 `.github/workflows/nightly-mpi-gpu.yml` 占位工作流。
