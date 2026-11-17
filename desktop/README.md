# D01 Linux 桌面引导

独立 CMake ≥3.20 / C++20 / AUTOMOC 工程，不接入根构建。默认启用
`CHIPCAE_BUILD_TESTS`。输出、PNG、CTest 日志均在调用者构建目录。

```bash
micromamba create -p <prefix> --file desktop/env/desktop-linux.lock.yml
export CMAKE_PREFIX_PATH="<prefix>"
export PATH="$CMAKE_PREFIX_PATH/bin:$PATH"
cmake -S desktop -B build-desktop -G Ninja -DCMAKE_PREFIX_PATH="$CMAKE_PREFIX_PATH"
cmake --build build-desktop --parallel 2
QT_QPA_PLATFORM=offscreen ctest --test-dir build-desktop --no-tests=error --output-on-failure
# 有 Mesa EGL ICD 时（本机常见）：无 DISPLAY 的离屏 EGL
env -u DISPLAY QT_QPA_PLATFORM=offscreen VTK_DEFAULT_OPENGL_WINDOW=vtkEGLRenderWindow \
  ctest --test-dir build-desktop --no-tests=error -R 'desktop_.*(window|render|capture|chart)' --output-on-failure
# GitHub ubuntu-latest 无 /dev/dri，vtkEGLRenderWindow 无法初始化设备。
# Desktop job 用 Xvfb + Mesa llvmpipe + vtkXOpenGLRenderWindow 做真实软件渲染，不算 skip。
xvfb-run -a env LIBGL_ALWAYS_SOFTWARE=1 VTK_DEFAULT_OPENGL_WINDOW=vtkXOpenGLRenderWindow \
  ctest --test-dir build-desktop --no-tests=error --output-on-failure -R desktop_render_smoke
# 真正嵌入 Qt OpenGL：在 X11 桌面运行，或用 Xvfb/Mesa
QT_QPA_PLATFORM=xcb xvfb-run -a build-desktop/desktop_window_smoke --present
build-desktop/chipcae-desktop
```

Qt offscreen 测试只构造真实 MainWindow/QVTK 控件并运行事件循环，不显示控件；
它不证明 Qt OpenGL 可用。独立 render 测试按 `VTK_DEFAULT_OPENGL_WINDOW`
选择 `vtkEGLRenderWindow`（默认，无 DISPLAY）或 `vtkXOpenGLRenderWindow`
（CI / Xvfb），渲染几何并重读 `desktop-render.png`，检查 320×240 和非恒定像素。
先查 SupportsOpenGL 再 Render，避免 EGL 初始化失败后段错误；GL 失败不跳过。
`--present` 为额外 X11 嵌入冒烟；手工旋转/缩放仍需有交互桌面的人复核。

## SDK 锁与探测

安装时将上面的 <prefix> 替换为所选目录。
`env/desktop-linux.lock.yml` 是原生 environment YAML，来自
`micromamba env export -p <prefix>`，移除了本机 name/prefix。
channels 为 conda-forge，dependencies 逐包固定 `name=version=build`，
适用于 linux-64，不包含 jsoncons conda 包。使用 YAML 是为了兼容 micromamba
对 .yml 扩展名的解析；包 URL 和 SHA256 另存于 sdk.json。
通过 PATH 选择 SDK 编译器，通过 CMAKE_PREFIX_PATH 查找包；也可传
`-DCHIPCAE_SDK_PREFIX=<prefix>` 或同名环境变量指定包查找前缀。
首次配置前仍需设置 PATH。

配置要求 GNU 15.3.0，并通过编译探测确认 libstdc++ C++11 ABI。
不比较编译器绝对路径，已删除逐文件 SDK 哈希校验（安装重定位会改变文件）。
锁固定 Qt 6.12.0、VTK 9.7.2 和 Protobuf CMake 35.1.0（conda 包 7.35.1）。
`env/desktop-linux.sdk.json` 仅为包版本/SHA256 与历史探测旁证，不参与配置。
`env/record_sdk.py <prefix>` 可在有意升级并完成复验后重新导出锁和包元数据。

jsoncons 1.4.0 头文件、原有相对路径 CMake config 和 Boost Software License 1.0
文本位于 `third_party/jsoncons/`，CMake 固定从此 vendor 目录查找。
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
不生成 gRPC。protoc 使用同一 Protobuf 包导出 target，包版本由 environment SDK 锁固定。

`workers/PreviewWorker.hpp`：Qt 所属线程内 `start(program, QStringList argv)`，
不经 shell；忙时返回 false。stdout/stderr 为独立 QByteArray 行信号（保留原字节，
去 LF/CRLF），跨 read 半行缓存，退出前发出尾部无换行片段，然后 finished。
启动/运行错误单独发 errorOccurred；FailedToStart 依 Qt 约定没有 finished。
`cancel(ms)` 先 terminate 后定时 kill，析构回收子进程。调用者自行判断 JSONL、
数字退出码、错误去重和取消状态；不在此 worker 内解释协议。不限制行长，调用者应
仅运行可信预览工具。测试覆盖分流、拆行、末尾片段、启动失败、取消和 worker 重用。

## 修复后验收记录（2026-11-17）

全新 Ninja configure/build 成功，parallel=2；全量 CTest 5/5、无 DISPLAY
且 offscreen/EGL 的 window/render 2/2，均无 skip。
构建文件确认 jsoncons include 来自仓库 vendor，不使用外部 jsoncons 前缀。
许可证策略和 clang-format dry-run（包括 vendor；vendor 保留上游格式）通过。
工作树 desktop 文件不再包含原本的两处本机 SDK/jsoncons 绝对路径。

锁文件的 micromamba create --file 安装 dry-run 成功，计划安装 283 个包；
此次没有执行完整重装。原生 explicit 导出在当前 micromamba 下因 .yml
扩展名被按 YAML 解析而失败，因此最终采用逐包 version=build 的 environment YAML。

协议兼容检查失败：当前 origin/main 已含错误码 11 MATERIAL_ID_DUPLICATE，
本分支 HEAD 的 protocol/errors.yaml 只有 1–10，工作树该文件与 HEAD 一致。
这是已有基线差异；本次限定只修改 desktop，未改协议或放宽检查。
CMake 的 VulkanHeaders/ZLIB/PNG/freetype 有可选探测提示，构建和 PNG 渲染测试通过。
人工交互旋转/缩放不在本次自动验收范围内。
