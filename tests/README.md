# tests

单元测试、集成测试和验证测试。

- `unit/`：不依赖外部求解库的快速测试，在每个 PR 的 GitHub Actions 上运行。
- 集成测试和验证算例将逐步加入，并与 `cases/` 中的基准数据对应。
- 超过 2 个 rank 的 MPI、GPU 和性能基线不在默认 CI 里运行。它们在 `.github/workflows/nightly-mpi-gpu.yml`，由仓库变量 `TIER_B_ENABLED` 关掉，见 `docs/ci.md`。
- 性能门槛和 CPU/GPU 一致性的单测在 `tools/perf/` 与 `tools/consistency/`，由 `CI / Lint` 运行。
