# tests/vv

- `test_pw_reference.py`：冻结 `tools/vv/pw_reference.py` 中 `run(..., 5000, 1600)` 的参考值：16.32000 / 16.24540 与 10.44021 / 9.35481 MPa（绝对容差 5e-5）。
- `test_yaml_numeric.py`：`yaml.safe_load` `cases/benchmarks` 下全部 YAML；任何字符串不得「像数字」（YAML 1.1 把 `3.10e10` 读成字符串，卡面须写 `3.10e+10`），材料卡数值字段（模量、α、ν、τ、C1/C2、温度、infinity、terms）必须是 int/float（允许占位 `tbd`）。
- `test_prony_scale.py`：`tools/vv/prony_scale.py` 的不变量——只缩放 E_i、τ_i 不变、E0 不变、E∞(s) = E0 − s(E0 − E∞_ref)、s = 0 ⇔ E ≡ E0、三端点卡合法且不含 s 字段；`I25D_UBUMP_UF_KIM2016` 与 `derive_cards.kim2016_homogenized` 一致；`tools.vv.*` 可按包导入。

运行上述检查可用 `python -m pytest -q tests/vv`（需要 pytest、pyyaml）。上述检查尚未并入仓库 CI。

## V&V 结果与 D03 入库检查

`tools/vv/schema/vv-result-0.2.schema.json` 定义 `chipcae.vv_result/0.2` 的结果结构。
按 D03，公开仓库仅接收 `data_class` 为 `synthetic` 或 `public` 的结果。
`real` 是合法的 schema 分类，真实案例结果必须留在本地。

```sh
python3 -m tools.vv.check_result_data_class
python3 -m tools.vv.check_result_data_class --repo . tests/vv/data/result_schema/valid_synthetic.json
python3 -m unittest tests.vv.test_result_schema -v
ctest --test-dir build -R '^vv_result_schema$' --output-on-failure
```

不传文件时，检查器从 Git 索引中选出文件名恰为 `result.json` 的路径，读取工作树中的文件。
空扫描输出 `0 result.json checked`。显式指定的文件可以使用其他文件名，相对路径以仓库根目录为准。
默认通过 Git 确定仓库根目录，也可用 `--repo` 指定。
解析符号链接和 `..` 后，文件必须在仓库内的 `cases/benchmarks/` 或 `tests/vv/data/` 下，
大小不得超过 2 MiB。指向同一解析路径的参数只检查一次。

退出码 `0` 表示通过，`1` 表示存在违规（每条以 `<path>: <reason>` 写入 stderr），
`2` 表示用法或 Git 仓库发现错误。检查器只使用 Python 标准库。
`vv_result_schema` 中只有 schema 与检查器的一致性测试需要可选的 `jsonschema`，
缺包时该项跳过，其余测试照常运行。

## F-1130 一键彩排

驱动只用 Python 标准库，使用已有合成夹具，固定按以下顺序报告：
`f1130_uncut_cli_e2e`、`f1130_uncut_cli_records`、
`chain_ipc_uncut_chain`、`odbcli_odbpp_cli_e2e`。

```sh
python -m tools.vv.f1130_rehearsal --build-dir "$BUILD_DIR" --out "$OUT_DIR" --ranks 1,2
```

`BUILD_DIR` 必须是已构建的 Release 目录，四个缓存项
`CHIPCAE_ENABLE_ECAD`、`CHIPCAE_ENABLE_MFEM`、`CHIPCAE_ENABLE_MPI`、
`CHIPCAE_ENABLE_HDF5` 均须开启。驱动保留缓存原值，接受标准 CMake 真值
（ON/YES/TRUE/Y、非零数值，大小写不敏感），缺项与假值均列出诊断。
MPI 仅用 `shutil.which("mpiexec")` 查找，`mpirun` 不作替代。
输出目录建立后，驱动先读取 Git commit，再检查构建目录、缓存和依赖；
因此缺缓存、后端或 mpiexec 的前检报告也保留 commit。
Git 启动失败、返回非零或没有输出时，commit 为 null，记录原始诊断并退出 2。
这四个开关不保证运行依赖完整；实测的求解流水线还需要启用 JSONCONS，
测试解释器需有 NumPy、h5py。实际缺依赖时的失败或跳过会保留在报告中。
先以 `ctest --test-dir "$BUILD_DIR" -N` 检查四个名字的精确注册，
再以以下 argv 执行（不经 shell）：

```sh
ctest --test-dir "$BUILD_DIR" -R '^(f1130_uncut_cli_e2e|f1130_uncut_cli_records|chain_ipc_uncut_chain|odbcli_odbpp_cli_e2e)$' -j2 --output-on-failure --output-junit "$OUT_DIR/junit.xml"
```

`--ranks` 默认 `1,2`，接受仅由 `1`、`2` 组成的非空逗号列表；空项、其他整数和
其他字符串均报参数错。该参数只改变报告元数据，各测试保持自身的 rank 行为。

退出码与判定：

- `0`：`PASS`，四个精确匹配且各只有一个 JUnit 结果，均为 `status="run"`，
  没有 failure/error/skipped，CTest 返回 0，且不存在依赖缺口。
- `1`：有效 XML 含 Failed、Skipped 或 Not Run，或 CTest 返回非零，报告 `FAIL`。
  failure/error 优先记 Failed，skipped 记 Skipped，其余非 run（含缺失 status）记
  Not Run。缺失、空或损坏的 JUnit 报告 `NOT_RUN`，保留 CTest 实际退出码；
  启动 CTest 失败时退出码为 null。缺失或重复的 testcase 不会被默认为通过。
- `2`：参数、目录、缓存、Git 来源、发现或报告写入失败。普通模式下缺后端或
  mpiexec 在任何 CTest 调用之前退出；发现失败或缺精确名字则只调用发现，
  不执行测试。输出目录可写时，操作性前检失败仍写 `NOT_RUN` 诊断报告。

`--allow-missing-backend` 允许带依赖缺口发现并执行测试，仍不能 PASS。
每项缺口在 `preflight_errors` 和状态为 Skipped 的 `readiness_gaps` 中保留。
按现有 CMake 注册条件，缺任何后端会阻止三个 e2e 的可执行文件注入；
若它们的 XML 仍为 Passed，则有效状态记 Skipped，原始 status 属性保留为 `junit_status`，
并附 readiness 原因。已有 Failed、Skipped、Not Run 不被覆盖。
records 测试没有后端要求。mpiexec 缺失只作为全局 readiness 缺口，
不推断每个测试与 MPI 的依赖关系；各项实际判定仍来自 XML。

报告 UTF-8 文件为 `rehearsal.json`、`rehearsal.md`；原始结果为 `junit.xml`。
只覆盖这三个文件，执行前移除旧的 `junit.xml`，避免复用旧通过结果。
其他文件保持不变。并发调用须使用不同输出目录。
Markdown 表格按清单排序并转义竖线和换行，围栏保留完整诊断原文，
并附完整 JSON，使两种格式包含相同信息。

JSON 字段：

| 字段 | 内容 |
| --- | --- |
| commit / build_dir | 脚本仓库根目录的 `git rev-parse HEAD` / 解析后的构建路径；未取得 commit 时为 null |
| backends | 上述四个完整 cache key 对应原始字符串；缺项为 null |
| mpiexec | 查找得到的路径，未找到为 null |
| ranks / allow_missing_backend | 请求的 rank 列表 / 是否启用允许缺口模式 |
| preflight_errors | 所有前检错误和 readiness 缺口 |
| ctest_listing / ctest_listing_returncode | 发现的 stdout、stderr 原文 / 退出码；未调用时为 null |
| ctest_command / ctest_returncode | 执行 argv 数组 / 实际退出码；未执行时为空数组 / null |
| tests | 清单顺序的 name、status、time_s、skip_or_fail_message；未测时间为 null |
| wall_time_s | monotonic 总墙钟秒数，包含共享构建槽的排队时间，不是纯求解耗时 |
| data_class / overall | 固定 synthetic / PASS、FAIL、NOT_RUN |

附加证据包括执行 stdout/stderr `ctest_output`、有效 XML 原文 `junit_xml`、
无法读取 XML 时的 `junit_error` 和允许缺口模式下的 `readiness_gaps`。
CTest 执行返回非零时，`ctest_error` 明确记录退出码，并显示在 Markdown 表格前；
各测试仍保留 JUnit 的实际状态，即使四项均 Passed，整体也会因 CTest 失败而 FAIL。
每项的诊断保留 testcase 属性、failure/error/skipped 属性与文本、system-out/err。
此报告不是 `chipcae.vv_result/0.2` schema，不对它调用 `check_file()`。

验收单元测试（临时目录、mock 子进程，不需要后端）：

```sh
python -m py_compile tools/vv/f1130_rehearsal.py tests/vv/test_f1130_rehearsal.py
python -m unittest tests.vv.test_f1130_rehearsal -v
ctest --test-dir "$BUILD_DIR" -N -R '^vv_f1130_rehearsal_unit$'
ctest --test-dir "$BUILD_DIR" -R '^vv_f1130_rehearsal_unit$' --output-on-failure
```

实测前查看 `MemAvailable`，低于 4 GB 时使用指定 Mac 自托管环境。
共享构建环境须保留 shim 的 PATH 优先级，再追加依赖环境 bin，并设置
`LIBRARY_PATH` 为依赖环境 lib；不绕过包装器，驱动自身提供 `-j2`。
记录 commit、完整命令、退出码和实际结果，报告标「实测」，不将 Skipped 当 Passed。
负向实测用保留 shim 但排除所有 mpiexec 目录的受控 PATH，预期退出 `2`。
仓库内不保存本地环境绝对路径或真实算例数据。

在上述环境就绪后，实测命令如下。两个输出目录须不同，以同时保留正常与负向证据。
先保存解释器路径，负向验证移除依赖 bin 后仍使用同一个解释器。

```sh
python -m tools.vv.f1130_rehearsal --build-dir "$BUILD_DIR" --out "$OUT_DIR" --ranks 1,2
PYTHON_EXE="$(command -v python)"
NO_MPI_PATH="$("$PYTHON_EXE" -c 'import os; from pathlib import Path; print(os.pathsep.join(p for p in os.environ["PATH"].split(os.pathsep) if p and not (Path(p) / "mpiexec").exists()))')"
PATH="$NO_MPI_PATH" "$PYTHON_EXE" -m tools.vv.f1130_rehearsal --build-dir "$BUILD_DIR" --out "$NEGATIVE_OUT_DIR" --ranks 1,2
```

负向报告应为 `NOT_RUN`，保留 commit，四项均 `Not Run`，发现和执行均未调用。
正常实测的 JSON、Markdown 和 JUnit 应随交付记录留存；实测红项/跳过原文交由对应模块处理，
不修改本卡范围外的集成断言或数值门值。

## CTest SKIP 审计

`tools.vv.ctest_skip_audit` 只用 Python 标准库，从一份 CTest JUnit 和对应构建目录的
`CMakeCache.txt` 汇总 Passed、Failed、Skipped、NotRun，不启动 CTest。

```sh
python3 -m tools.vv.ctest_skip_audit --junit build/junit.xml \
  --build-dir build --out build/skip-audit --required tests/vv/skip_audit_required.tsv
python3 -m unittest tests.vv.test_ctest_skip_audit -v
ctest --test-dir build -R '^vv_ctest_skip_audit_unit$' --output-on-failure
```

输出 `skip-audit.json` 与 `skip-audit.md`；已有输出目录中仅覆盖这两个文件。
退出码 0 表示没有启用的必跑项被跳过、未运行或缺失，1 表示存在这些违规，2 表示参数、
输入或输出读写错误。普通 Failed 仍按 Failed 计数，不产生 skip 违规；必须同时保留
原始 CTest 的退出码。省略 `--required` 时仅汇总，合法输入的审计退出码为 0。

必跑清单为三列 TSV：`test_name<TAB>requires<TAB>note`，忽略空行和 `#` 注释。
`requires` 为 `-` 时无条件必跑，否则其中逗号分隔的所有 BOOL 开关必须开启。
CMake 的 ON/TRUE/YES/Y/1 等真值不区分大小写；OFF/FALSE/NO/N/0、空值、IGNORE、
NOTFOUND 和 `*-NOTFOUND` 为关闭。缓存中缺少的开关按 OFF 处理并给出警告。
缺失测试始终列在 `missing_required`，只在启用时违规；未启用项列出 OFF/缺失开关。
生产清单固定为六项，增删须由 vv 裁定。

四条 CLI 链路的 CMake 环境门控均需 ECAD、MFEM、MPI、HDF5，且需要相应程序 target：
`f1130_uncut_cli_e2e`、`chain_ipc_uncut_chain`、`odbcli_odbpp_cli_e2e`、
`fvm_fvm_case_e2e`。`field_compare_ranks` 按 brief 仅以 MPI 为必跑门控，
但它的实际注册还依赖 MFEM、HDF5 和 solver target，因此 MPI 单独开启时可能报 Missing。
`vv_result_schema` 无条件必跑。审计器只根据清单中的 BOOL 判断，不推断 target 是否存在。

支持 `<testsuite>`/`<testsuites>` 根，精确测试名重复时按文档顺序取最后一次，并警告次数。
Disabled 优先归 NotRun；其他带 `<skipped>` 的测试归 Skipped，优先于 `status="notrun"`；
然后依次判断 notrun、failure/fail、成功 run。未知状态或无效 XML 结构报输入错误。
原因优先取 skipped 的非空 message，其次取文本，再取 system-out 原文；不解析控制台文字
判断状态。原因最多保留 500 字符，JSON 保留原始空白，Markdown 对表格换行、竖线做显示转义。
Skipped 的 `time` 写为 `elapsed_seconds`，缺失、非有限或负值写 null 并警告。

报告记录所有 `CHIPCAE_ENABLE_*` BOOL 的原始值与启用状态，以及 `shutil.which("mpiexec")`。
提交取缓存 `CMAKE_HOME_DIRECTORY:INTERNAL` 所指源码的当前 HEAD；发现失败写 null 和警告，
不会回退到审计器所在仓库。该 HEAD 可能不同于原构建时提交，复用旧 JUnit 时应另外记录
构建来源与时间。`data_class` 固定为 synthetic，报告列表按名称排序。
35 个验收测试使用合成样本及临时 Git 仓库，包括 500 条测试在 5 秒内完成的检查。

## 验证记录（2026-11-22）

在 `671767ba4629e513e443eec325ee689850c85bc4` 加本次修复的工作树上实测。
`BUILD_DIR=build-v-int-f1130-rehearsal-full-release`，缓存为 Release，
ECAD、MFEM、MPI、HDF5、JSONCONS 均 ON；共享包装器保留在 PATH 首位。
正常实测前 `MemAvailable=7061444 kB`。`OUT_DIR`、`NEGATIVE_OUT_DIR`
为独立的本地证据目录，保留 JSON、Markdown，正常目录另有 JUnit。

| 命令 | 退出码 | 实际结果 |
| --- | --- | --- |
| `cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release` | 0 | Release 配置完成；已有依赖 CMake 警告保留在日志中 |
| `cmake --build "$BUILD_DIR" -j2` | 0 | 构建完成 |
| `python -m py_compile tools/vv/f1130_rehearsal.py tests/vv/test_f1130_rehearsal.py` | 0 | 语法检查通过 |
| `python -m unittest tests.vv.test_f1130_rehearsal -v` | 0 | 31/31 通过 |
| `ctest --test-dir "$BUILD_DIR" -N -R '^vv_f1130_rehearsal_unit$'` | 0 | 精确发现 1 项 |
| `ctest --test-dir "$BUILD_DIR" -R '^vv_f1130_rehearsal_unit$' --output-on-failure` | 0 | 1/1 Passed |
| `python -m tools.vv.f1130_rehearsal --build-dir "$BUILD_DIR" --out "$OUT_DIR" --ranks 1,2` | 0 | PASS；CTest 退出 0，四项均 Passed |
| `ctest --test-dir "$BUILD_DIR" -j1 --output-on-failure --output-junit "$FULL_JUNIT"` | 8 | 单次全量：441 Passed、8 Failed、4 Timeout、27 Skipped、10 Disabled（共 490 项） |
| `PATH="$SHIM_DIR:$NO_MPI_TOOLS:$NO_MPI_PATH" "$PYTHON_EXE" -m tools.vv.f1130_rehearsal --build-dir "$BUILD_DIR" --out "$NEGATIVE_OUT_DIR" --ranks 1,2` | 2（预期） | `mpiexec not found on PATH`；NOT_RUN，保留 commit，未发现或执行 CTest |

负向实测的 `SHIM_DIR` 指已有共享包装器目录，`NO_MPI_PATH` 排除所有含
mpiexec 的目录；`NO_MPI_TOOLS` 是只含 Git 符号链接的临时目录，以保留 Git。
回归先红后绿：新增诊断断言在修复前令相关 CTest 退出 8（31 项中 1 项失败），
修复后同一相关测试选择退出 0（1/1 Passed）。未减少既有断言。
本次只改 Python 与文档；clang-format 23.1.5 的 `-i`、`--dry-run --Werror`
按改动的 C/C++ 文件选择执行，均无适用输入。

单次全量耗时 8171.65 s，未达到全绿要求；`FULL_JUNIT` 为本地全量 JUnit 路径。
本卡单元与四个彩排测试在全量中仍全部 Passed。全量失败/超时名称如下，
未更改其断言、容差、跳过条件或超时设置：

- `solve_ghost_scan`
- `solve_demo`
- `solve_column_gmg_fcbga_scaling`
- `solve_fem_thermal_bow`
- `contract_output_solver`
- `cutbim_cut_bimetal`
- `field_compare_fem_fvm`
- `field_compare_outline_bbox`
- `field_compare_ranks`
- `fvm_fvm_case_e2e`
- `ecaddiff_errors`
- `preview_region_planes`

## 许可

`tools/vv/` 与 `tests/vv/` 按 Apache-2.0 发布（见各目录 LICENSE）。
