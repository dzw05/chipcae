# D01 Linux 桌面引导

独立 CMake ≥3.20 / C++20 / AUTOMOC 工程，不接入根构建。默认启用
`CHIPCAE_BUILD_TESTS`。输出、PNG、CTest 日志均在调用者构建目录。

```bash
export CMAKE_PREFIX_PATH=/workspace/tools/chipcae-desktop
export PATH="$CMAKE_PREFIX_PATH/bin:$PATH"
cmake -S desktop -B build-desktop -G Ninja -DCMAKE_PREFIX_PATH="$CMAKE_PREFIX_PATH"
cmake --build build-desktop --parallel 2
QT_QPA_PLATFORM=offscreen ctest --test-dir build-desktop --no-tests=error --output-on-failure
env -u DISPLAY QT_QPA_PLATFORM=offscreen VTK_DEFAULT_OPENGL_WINDOW=vtkEGLRenderWindow \
  ctest --test-dir build-desktop --no-tests=error -R 'desktop_.*(window|render|capture|chart)' --output-on-failure
QT_QPA_PLATFORM=offscreen ctest --test-dir build-desktop --no-tests=error --output-on-failure \
  -R 'desktop_(window|render)_smoke'
# 真正嵌入 Qt OpenGL：在 X11 桌面运行，或用 Xvfb/Mesa
QT_QPA_PLATFORM=xcb xvfb-run -a build-desktop/desktop_window_smoke --present
build-desktop/chipcae-desktop
```

Qt offscreen 测试只构造真实 MainWindow/QVTK 控件并运行事件循环，不显示控件；
它不证明 Qt OpenGL 可用。独立 render 测试显式使用 vtkEGLRenderWindow，
渲染几何并重读 `desktop-render.png`，检查 320×240 和非恒定像素；GL 失败不跳过。
`--present` 为额外 X11 嵌入冒烟；手工旋转/缩放仍需有交互桌面的人复核。

## SDK 锁与探测

初次共享安装因 conda-forge 无 jsoncons 包失败；去掉该包并显式补 vtk-base 后成功。
没有使用 pip SDK、ParaView 前缀、源码 VTK 或版本降级。安装命令：

```bash
/workspace/tools/bin/micromamba create -y -p /workspace/tools/chipcae-desktop \
  -c conda-forge --strict-channel-priority 'qt6-main>=6.5,<7' 'vtk=9.7.*' vtk-base \
  cmake ninja cxx-compiler libprotobuf protobuf python h5py numpy
```

实际 Qt 6.12.0、VTK 9.7.2，C++ 头文件/CMake/GUISupportQt/IOHDF/
ChartsCore/ViewsContext2D/RenderingContextOpenGL2 均存在，后面三项供 D12 使用。
GNU 15.3.0、x86_64-conda-linux-gnu、libstdc++ C++11 ABI；编译/链接成功，
ldd 中 Qt、VTK、libstdc++ 均来自同一 SDK（系统 glibc/GL 驱动除外）。
protoc 输出 `libprotoc 35.1`，CMake protobuf 35.1.0，conda libprotobuf/protobuf
均 7.35.1（上游包与 CMake 版本记法不同），生成代码/runtime 版本检查和往返测试覆盖配对。

`env/desktop-linux.lock.yml` 是 YAML 1.2 兼容 JSON：包含全部 conda 包的版本、
build string、下载 URL、SHA256 及关键 so/CMake/编译器哈希。
`cmake/SDKLock.cmake` 在配置时校验锁定文件，且拒绝外部编译器/Qt/VTK/Protobuf 配置。
此锁用于本机绝对路径；迁移时按 URL/哈希重建 prefix 后审核更新路径。
`env/record_sdk.py` 可重新记录，但只应在有意升级 SDK 并完成全部复验后运行，不能用于绕过漂移失败。

jsoncons 1.4.0 复用 `/workspace/deps/jsoncons`，Boost Software License 1.0，
头文件与 CMake 全量哈希已入锁；`CHIPCAE_JSONCONS_PREFIX` 指定查找位置。
实测 draft 2020-12 的 prefixItems、items:false、allOf 配合
unevaluatedProperties:false，合法/非法实例均断言；这不是完整官方 schema 测试套件认证。

直接链接的 Qt 模块为 Core、Gui、Widgets、OpenGLWidgets；自动依赖 OpenGL、
Linux 平台支持/DBus（ldd 实测）。这些模块按 LGPL-3.0-only（或商业许可）使用，
动态链接；发布仍须附 Qt 对应许可证、源码获取/重链接所需材料。无 Qt Charts/Qt GRPC。
VTK Charts 使用 VTK BSD-3-Clause。

## 复用接口

`cmake/ProtocolGeneration.cmake` 的 `chipcae_generate_protocol()` 导出
`chipcae_desktop_protocol` 静态 target，公开 generated include 根和 libprotobuf。
从仓库 protocol 根生成 run.v1/jobs.v1，依赖二者及 timestamp/duration 输入；
well-known 实现由匹配的 libprotobuf 提供，不重复生成。D11/D12 链此 target 即可，
不生成 gRPC。protoc 使用同一 Protobuf 包导出 target，文件哈希由 SDK 锁校验。

`workers/PreviewWorker.hpp`：Qt 所属线程内 `start(program, QStringList argv)`，
不经 shell；忙时返回 false。stdout/stderr 为独立 QByteArray 行信号（保留原字节，
去 LF/CRLF），跨 read 半行缓存，退出前发出尾部无换行片段，然后 finished。
启动/运行错误单独发 errorOccurred；FailedToStart 依 Qt 约定没有 finished。
`cancel(ms)` 先 terminate 后定时 kill，析构回收子进程。调用者自行判断 JSONL、
数字退出码、错误去重和取消状态；不在此 worker 内解释协议。不限制行长，调用者应
仅运行可信预览工具。测试覆盖分流、拆行、末尾片段、启动失败、取消和 worker 重用。

## 本机验收记录（2026-11-17）

从工作树根实际运行上述 configure/build：成功；全量 CTest 5/5，去 DISPLAY
筛选 window/render/capture/chart 2/2，专项 window/render 2/2，无 skip。
EGL 日志：`vtkEGLRenderWindow PNG 320x240 range 13:234`。
Xvfb `--present` 显示嵌入窗口并正常退出，无 OpenGL 错误。
独立 render 可执行文件 ldd 无 Qt 依赖。编译器宏实测
`_GLIBCXX_USE_CXX11_ABI=1`、`__GXX_ABI_VERSION=1020`。
许可证策略、协议兼容检查和 clang-format 23.1.5 dry-run 均通过。
CMake 的 VulkanHeaders/ZLIB/PNG/freetype 可选探测有未找到提示，但所选 VTK
预编译 target 的实际链接与 PNG 写读成功，未绕过测试。
尚未进行人工旋转缩放及外部交叉评审；无 SDK/GL 阻塞。
