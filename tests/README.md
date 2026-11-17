# tests

单元测试、集成测试和验证测试。

- `unit/`：不依赖外部求解库的快速测试，在每个 PR 的 GitHub Actions 上运行。
- 集成测试和验证算例将逐步加入，并与 `cases/` 中的基准数据对应。
- 超过 2 个 rank 的 MPI、GPU 和性能基线不在默认 CI 里运行。它们在 `.github/workflows/nightly-mpi-gpu.yml`，由仓库变量 `TIER_B_ENABLED` 关掉，见 `docs/ci.md`。
- 性能门槛和 CPU/GPU 一致性的单测在 `tools/perf/` 与 `tools/consistency/`，由 `CI / Lint` 运行。
- 新测试自动收集：新建 `tests/<区域>/<名字>/CMakeLists.txt`，只写一行 `chipcae_auto_tests(LIBS <依赖目标> [LABELS ...] [TIMEOUT 秒])`，同目录放一个空的标记文件 `.chipcae-autotests`，再放入 `test_*.cpp` / `test_*.py`。每个 `test_*.cpp` 生成一个可执行并 add_test，测试名为文件名去掉 `test_` 前缀；`test_*.py` 用 Python3 运行（第 1 个参数是工程根目录），SKIP_RETURN_CODE 77；`test_occt_*` 只在 CHIPCAE_ENABLE_OCCT=ON 时收集并带 occt 标签。该目录由 `tests/CMakeLists.txt` 末行的 `chipcae_auto_test_subdirs()` 自动加入，不要再改 `tests/CMakeLists.txt`。实现见 `cmake/ChipcaeAutoTests.cmake`，自检见 `auto_selftest/`。

自动加入只扫描 `tests/*/CMakeLists.txt` 和 `tests/*/*/CMakeLists.txt`。如果父目录已有 `CMakeLists.txt`，嵌套目录由父目录负责加入；已显式加入的目录会跳过。
- 自动收集的测试名在 CTest 里全局唯一：文件名可能重名时用 `PREFIX <区域>_`；需要进 `-L tierA` / `-L mesh` 等按标签过滤的任务时写 `LABELS`。C++ 测试编译时带 `CHIPCAE_SOURCE_DIR` / `CHIPCAE_TEST_SOURCE_DIR` 宏；`return 77` 只对 Python 测试算跳过。
