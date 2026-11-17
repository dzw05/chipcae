# cmake

存放工程级 CMake 模块：

- `ChipcaeOptions.cmake`：可开关的依赖和构建选项，可选依赖默认关闭。
- `ChipcaeDependencies.cmake`：按选项查找第三方库。
- `ChipcaeLicensePolicy.cmake`：闭源商用的许可证策略，禁止引入 GPL 依赖。
- `ChipcaeWarnings.cmake`：统一的编译警告选项。
- `ChipcaeAutoTests.cmake`：测试自动收集（`chipcae_auto_tests` / `chipcae_auto_test_subdirs`），见 `tests/README.md`。
- `check_license_policy.py`：扫描构建脚本，拦截被排除的依赖名。

查找新依赖前，先核对其许可证是否在允许列表中。

依赖白名单见 `dependencies.allowlist`；新增依赖必须加入此文件并由 owner 审批。MUMPS 使用 `CHIPCAE_ENABLE_MUMPS`，默认 OFF，仅允许动态链接，作为可选直接求解后端。
