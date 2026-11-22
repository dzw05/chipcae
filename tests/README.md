# tests

单元测试、集成测试和验证测试。

- `unit/`：不依赖外部求解库的快速测试，在每个 PR 的 GitHub Actions 上运行。
- 集成测试和验证算例将逐步加入，并与 `cases/` 中的基准数据对应。
- 超过 2 个 rank 的 MPI、GPU 和性能基线不在默认 CI 里运行。它们在 `.github/workflows/nightly-mpi-gpu.yml`，由仓库变量 `TIER_B_ENABLED` 关掉，见 `docs/ci.md`。
- 性能门槛和 CPU/GPU 一致性的单测在 `tools/perf/` 与 `tools/consistency/`，由 `CI / Lint` 运行。
- 新测试自动收集：新建 `tests/<区域>/<名字>/CMakeLists.txt`，只写一行 `chipcae_auto_tests(LIBS <依赖目标> [LABELS ...] [TIMEOUT 秒])`，同目录放一个空的标记文件 `.chipcae-autotests`，再放入 `test_*.cpp` / `test_*.py`。每个 `test_*.cpp` 生成一个可执行并 add_test，测试名为文件名去掉 `test_` 前缀；`test_*.py` 用 Python3 运行（第 1 个参数是工程根目录），SKIP_RETURN_CODE 77；`test_occt_*` 只在 CHIPCAE_ENABLE_OCCT=ON 时收集并带 occt 标签。该目录由 `tests/CMakeLists.txt` 末行的 `chipcae_auto_test_subdirs()` 自动加入，不要再改 `tests/CMakeLists.txt`。实现见 `cmake/ChipcaeAutoTests.cmake`，自检见 `auto_selftest/`。

自动加入只扫描 `tests/*/CMakeLists.txt` 和 `tests/*/*/CMakeLists.txt`。如果父目录已有 `CMakeLists.txt`，嵌套目录由父目录负责加入；已显式加入的目录会跳过。
- 自动收集的测试名在 CTest 里全局唯一：文件名可能重名时用 `PREFIX <区域>_`；需要进 `-L tierA` / `-L mesh` 等按标签过滤的任务时写 `LABELS`。C++ 测试编译时带 `CHIPCAE_SOURCE_DIR` / `CHIPCAE_TEST_SOURCE_DIR` 宏；`return 77` 只对 Python 测试算跳过。

每项测试必须恰有一个分层标签 `unit | module | highmem | perf | acceptance` 和一个
`owner:<MODULE>` 标签（`M1`–`M7`、`INT`、`ARCH`、`INFRA`）。集中规则与归属表见
[`ChipcaeTestTiers.cmake`](../cmake/ChipcaeTestTiers.cmake)：优先按已知源文件的最长前缀
匹配 owner，否则用注册目录及手工注册测试的名称规则；新目录需在表中明确归属。
配置时检查标签，`ctest --test-dir build -R '^ctest_tier_labels$'` 再检查完整 CTest 清单。
CMake < 3.28 会警告并跳过自动赋标签，仍需通过该 CTest 检查。

新增测试：秒级检查选 `unit`，数值/MMS/收敛/MPI/模块集成或 TIMEOUT ≥ 120 秒选
`module`，计时/吞吐选 `perf`，端到端/真实算例/install/nightly 选 `acceptance`。
显式指定一个分层标签时保留，多个标签会报错；未指定时依次按 perf、acceptance、module、
unit 推断。自动收集默认 TIMEOUT 为 120 秒，因此默认归入 module。
`highmem` 仅保留已测量且使用 `chipcae_highmem` 锁的三个测试，不得扩大集合。
已有主题标签（如 `numerical`、`mesh`）保留。

PR 托管 CI 先跑 `-L '^(unit|module)$'`，再跑 `-LE '^(unit|module)$'`，保持现有合并门禁
覆盖所有已配置测试；Mac 每天北京时间 02:00 跑 highmem，03:00 跑 acceptance 和串行 perf。
使用 `gh workflow run mac-highmem.yml --ref <branch>` 可把分支的 highmem 检查交给 Mac。
