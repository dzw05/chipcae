# cmake

存放工程级 CMake 模块：

- `ChipcaeOptions.cmake`：可开关的依赖和构建选项，可选依赖默认关闭。
- `ChipcaeDependencies.cmake`：按选项查找第三方库。
- `ChipcaeLicensePolicy.cmake`：闭源商用的许可证策略，禁止引入 GPL 依赖。
- `ChipcaeWarnings.cmake`：统一的编译警告选项。
- `check_license_policy.py`：扫描构建脚本，拦截被排除的依赖名。

查找新依赖前，先核对其许可证是否在允许列表中。
